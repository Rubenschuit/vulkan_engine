#include "pch.hpp"
#include "resources/ve_texture.hpp"
#include "resources/internal/loaded_asset_data.hpp"
#include "resources/internal/upload_context.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_device.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

#include <cmath>
#include <filesystem>
#include <vector>
#include <ktx.h>
#include <ktxvulkan.h>

namespace {
	bool isBlockCompressed(vk::Format format) {
		std::string s = vk::to_string(format);
		return s.find("Block") != std::string::npos;
	}

	// Convert a block-compressed format to its sRGB variant
	vk::Format toSrgbBC(vk::Format f) {
		switch (f) {
			case vk::Format::eBc1RgbUnormBlock:  return vk::Format::eBc1RgbSrgbBlock;
			case vk::Format::eBc1RgbaUnormBlock: return vk::Format::eBc1RgbaSrgbBlock;
			case vk::Format::eBc3UnormBlock:     return vk::Format::eBc3SrgbBlock;
			case vk::Format::eBc7UnormBlock:     return vk::Format::eBc7SrgbBlock;
			case vk::Format::eAstc4x4UnormBlock: return vk::Format::eAstc4x4SrgbBlock;
			default: return f;
		}
	}

	// Convert a block-compressed format to its Unorm (linear) variant
	vk::Format toUnormBC(vk::Format f) {
		switch (f) {
			case vk::Format::eBc1RgbSrgbBlock:  return vk::Format::eBc1RgbUnormBlock;
			case vk::Format::eBc1RgbaSrgbBlock:  return vk::Format::eBc1RgbaUnormBlock;
			case vk::Format::eBc3SrgbBlock:      return vk::Format::eBc3UnormBlock;
			case vk::Format::eBc7SrgbBlock:      return vk::Format::eBc7UnormBlock;
			case vk::Format::eAstc4x4SrgbBlock:  return vk::Format::eAstc4x4UnormBlock;
			default: return f;
		}
	}

	bool isSrgbColorTexture(ve::TextureType type) {
		return type == ve::TextureType::ALBEDO
			|| type == ve::TextureType::EMISSIVE
			|| type == ve::TextureType::SPECULAR_COLOR;
	}

	ve::AlphaCoverage classifyAlbedoAlpha(const std::vector<uint8_t>& rgba8) {
		constexpr uint8_t OPAQUE_MIN = 250;
		constexpr uint8_t MID_LOW = 64;
		constexpr uint8_t MID_HIGH = 192;
		constexpr float SUB_OPAQUE_FRACTION = 0.005f;
		constexpr float GRADIENT_MID_FRACTION = 0.10f;

		const size_t texel_count = rgba8.size() / 4;
		if (texel_count == 0)
			return ve::AlphaCoverage::Unknown;

		size_t below_opaque = 0;
		size_t mid_band = 0;
		for (size_t i = 0; i < texel_count; i++) {
			uint8_t a = rgba8[i * 4 + 3];
			if (a < OPAQUE_MIN)
				below_opaque++;
			if (a >= MID_LOW && a <= MID_HIGH)
				mid_band++;
		}
		if (static_cast<float>(below_opaque) / static_cast<float>(texel_count) <= SUB_OPAQUE_FRACTION)
			return ve::AlphaCoverage::Opaque;
		if (static_cast<float>(mid_band) / static_cast<float>(texel_count) > GRADIENT_MID_FRACTION)
			return ve::AlphaCoverage::Translucent;
		return ve::AlphaCoverage::Cutout;
	}

	// Transcodes (if needed) and copies the ktxTexture's pixel data + format/mip
	// layout into a DecodedTexture
	bool populateDecodedFromKtx(ktxTexture* k_texture, vk::Format format_hint,
	                            const ve::GpuCaps& caps, bool is_albedo, ve::DecodedTexture& out) {
		out.width = k_texture->baseWidth;
		out.height = k_texture->baseHeight;
		if (out.width == 0 || out.height == 0)
			return false;
		out.is_cubemap = k_texture->isCubemap;
		out.array_layers = out.is_cubemap ? 6u : 1u;
		out.mip_levels = k_texture->numLevels;

		// Probe alpha from the component count before transcoding compresses it away
		if (is_albedo && k_texture->classId == ktxTexture2_c) {
			ktx_uint32_t components = ktxTexture2_GetNumComponents(reinterpret_cast<ktxTexture2*>(k_texture));
			out.alpha_coverage = (components >= 4) ? ve::AlphaCoverage::Cutout : ve::AlphaCoverage::Opaque;
		}

		if (k_texture->classId == ktxTexture2_c) {
			ktxTexture2* ktx2 = reinterpret_cast<ktxTexture2*>(k_texture);
			if (ktxTexture2_NeedsTranscoding(ktx2)) {
				ktx_transcode_fmt_e target = KTX_TTF_RGBA32;
				if (caps.supports_bc)
					target = KTX_TTF_BC7_RGBA;
				else if (caps.supports_astc)
					target = KTX_TTF_ASTC_4x4_RGBA;
				if (ktxTexture2_TranscodeBasis(ktx2, target, 0) != KTX_SUCCESS)
					return false;
			}
		}

		vk::Format texture_format = vk::Format::eR8G8B8A8Unorm;
		if (k_texture->classId == ktxTexture2_c) {
			auto* ktx2 = reinterpret_cast<ktxTexture2*>(k_texture);
			texture_format = static_cast<vk::Format>(ktx2->vkFormat);
			if (texture_format == vk::Format::eUndefined)
				texture_format = vk::Format::eR8G8B8A8Unorm;
		} else if (k_texture->classId == ktxTexture1_c) {
			texture_format = static_cast<vk::Format>(ktxTexture1_GetVkFormat(reinterpret_cast<ktxTexture1*>(k_texture)));
			if (texture_format == vk::Format::eUndefined)
				texture_format = vk::Format::eR8G8B8A8Unorm;
		}

		bool is_uncompressed_8bit = (texture_format == vk::Format::eR8G8B8Srgb ||
		                             texture_format == vk::Format::eR8G8B8Unorm ||
		                             texture_format == vk::Format::eR8G8B8A8Srgb ||
		                             texture_format == vk::Format::eR8G8B8A8Unorm);
		bool has_format_hint = (format_hint == vk::Format::eR8G8B8A8Srgb || format_hint == vk::Format::eR8G8B8A8Unorm);
		if (has_format_hint && is_uncompressed_8bit) {
			texture_format = format_hint;
		} else if (has_format_hint && isBlockCompressed(texture_format)) {
			bool want_srgb = (format_hint == vk::Format::eR8G8B8A8Srgb);
			texture_format = want_srgb ? toSrgbBC(texture_format) : toUnormBC(texture_format);
		}
		out.format = texture_format;

		const bool use_mipmap_copy = (out.mip_levels > 1);
		if (use_mipmap_copy) {
			out.mip_offsets.reserve(out.mip_levels);
			out.mip_extents.reserve(out.mip_levels);
			for (uint32_t level = 0; level < out.mip_levels; level++) {
				ktx_size_t offset = 0;
				ktxTexture_GetImageOffset(k_texture, level, 0, 0, &offset);
				out.mip_offsets.push_back(offset);
				uint32_t w = std::max(1u, out.width >> level);
				uint32_t h = std::max(1u, out.height >> level);
				out.mip_extents.push_back({ w, h, 1u });
			}
			ktx_size_t total = ktxTexture_GetDataSize(k_texture);
			const uint8_t* src = ktxTexture_GetData(k_texture);
			out.pixels.assign(src, src + total);
		} else {
			ktx_size_t image_size = ktxTexture_GetImageSize(k_texture, 0);
			size_t total = static_cast<size_t>(image_size) * static_cast<size_t>(out.array_layers);
			const uint8_t* src = ktxTexture_GetData(k_texture);
			out.pixels.assign(src, src + total);
		}
		return true;
	}
}

namespace ve {

VeTexture::VeTexture(VeDevice& ve_device, const std::string& resource_id)
	: Resource(resource_id), m_ve_device(ve_device) {
}

VeTexture::VeTexture(VeDevice& ve_device, uint32_t width, uint32_t height, TextureType type)
	: Resource("default"), m_ve_device(ve_device) {
	DecodedTexture decoded;
	decoded.width = width;
	decoded.height = height;
	decoded.format = (type == TextureType::ALBEDO) ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
	decoded.mip_levels = 1;
	decoded.array_layers = 1;
	stbi_uc* pixels = generateDefaultTexture(static_cast<int>(width), static_cast<int>(height), type);
	const size_t byte_count = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	decoded.pixels.assign(pixels, pixels + byte_count);
	free(pixels);

	SyncUploadScope scope(ve_device, std::max<vk::DeviceSize>(decoded.pixels.size(), 4096));
	if (!recordDecodedTexture(decoded, scope.ctx))
		return;
	createTextureSampler();
	setLoaded(true);
}

VeTexture::VeTexture(VeDevice& ve_device, const std::string& resource_id, const DecodedTexture& decoded)
	: Resource(resource_id), m_ve_device(ve_device) {
	SyncUploadScope scope(ve_device);
	if (!recordDecodedTexture(decoded, scope.ctx)) {
		VE_LOGE("VeTexture upload from decoded data failed: " << resource_id);
		return;
	}
	createTextureSampler();
	setLoaded(true);
}

VeTexture::VeTexture(VeDevice& ve_device, const std::string& resource_id, const DecodedTexture& decoded, UploadContext& ctx)
	: Resource(resource_id), m_ve_device(ve_device) {
	if (!recordDecodedTexture(decoded, ctx)) {
		VE_LOGE("VeTexture record from decoded data failed: " << resource_id);
		return;
	}
	createTextureSampler();
	setLoaded(true);
}

std::shared_ptr<VeTexture> VeTexture::createDefault(VeDevice& device, TextureType type) {
	return std::make_shared<VeTexture>(device, 4, 4, type);
}

ResourceHandle<VeTexture> VeTexture::loadFromPath(VeResourceManager& resource_manager,
                                                  const std::filesystem::path& path,
                                                  TextureType fallback_type) {
	return resource_manager.load<VeTexture>(makeResourceKey(path, fallback_type));
}

std::string VeTexture::makeResourceKey(const std::filesystem::path& path, TextureType type) {
	// Format hint has only two buckets currently
	const char* suffix = isSrgbColorTexture(type) ? "|srgb" : "|linear";
	return path.lexically_normal().generic_string() + suffix;
}

DecodedTexture VeTexture::decode(const std::filesystem::path& path, TextureType type,
                                 const GpuCaps& gpu_caps,
                                 const EmbeddedImageData* embedded) {
	DecodedTexture out;
	out.resource_id = path.lexically_normal().generic_string();
	out.file_path = path;
	out.type = type;

	vk::Format format_hint = isSrgbColorTexture(type)
	                         ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;

	if (embedded) {
		if (embedded->is_ktx) {
			ktxTexture* k = nullptr;
			KTX_error_code res = ktxTexture_CreateFromMemory(
				embedded->bytes.data(), embedded->bytes.size(),
				KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &k);
			if (res != KTX_SUCCESS) {
				VE_LOGE("KTX embedded load failed: " << out.resource_id);
				return out;
			}
			if (!populateDecodedFromKtx(k, format_hint, gpu_caps, type == TextureType::ALBEDO, out)) {
				ktxTexture_Destroy(k);
				return out;
			}
			ktxTexture_Destroy(k);
		} else {
			// Encoded image (PNG/JPEG/etc.): decode via stb
			int w = 0, h = 0, ch = 0;
			stbi_uc* pixels = stbi_load_from_memory(
				embedded->bytes.data(), static_cast<int>(embedded->bytes.size()),
				&w, &h, &ch, STBI_rgb_alpha);
			if (!pixels) {
				const char* reason = stbi_failure_reason();
				VE_LOGE("STB embedded decode failed: " << out.resource_id
					<< " (" << (reason ? reason : "unknown") << ")");
				return out;
			}
			out.width = static_cast<uint32_t>(w);
			out.height = static_cast<uint32_t>(h);
			out.format = format_hint;
			out.mip_levels = 1;
			out.array_layers = 1;
			size_t byte_count = static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4u;
			out.pixels.assign(pixels, pixels + byte_count);
			stbi_image_free(pixels);
			if (type == TextureType::ALBEDO)
				out.alpha_coverage = classifyAlbedoAlpha(out.pixels);
		}
		return out;
	}

	if (!std::filesystem::exists(path))
		return out;

	auto ext = path.extension().string();
	if (ext == ".ktx" || ext == ".ktx2") {
		ktxTexture* k = nullptr;
		KTX_error_code res = ktxTexture_CreateFromNamedFile(
			path.string().c_str(),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &k);
		if (res != KTX_SUCCESS) {
			VE_LOGE("KTX load failed: " << path.string() << " (error " << static_cast<int>(res) << ")");
			return out;
		}
		if (!populateDecodedFromKtx(k, format_hint, gpu_caps, type == TextureType::ALBEDO, out)) {
			ktxTexture_Destroy(k);
			return out;
		}
		ktxTexture_Destroy(k);
		return out;
	}

	int w = 0, h = 0, channels = 0;
	stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, STBI_rgb_alpha);
	if (!pixels) {
		const char* reason = stbi_failure_reason();
		VE_LOGE("STB load failed: " << path << " (" << (reason ? reason : "unknown") << ")");
		return out;
	}
	out.width = static_cast<uint32_t>(w);
	out.height = static_cast<uint32_t>(h);
	out.mip_levels = 1;
	out.array_layers = 1;
	out.format = format_hint;
	size_t byte_count = static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4u;
	out.pixels.assign(pixels, pixels + byte_count);
	stbi_image_free(pixels);
	if (type == TextureType::ALBEDO)
		out.alpha_coverage = classifyAlbedoAlpha(out.pixels);
	return out;
}

VeTexture::~VeTexture() {
	unload();
}

bool VeTexture::doLoad() {
	// Default placeholder textures
	struct DefaultEntry { const char* id; TextureType type; vk::Format format; };
	static constexpr DefaultEntry defaults[] = {
		{"default_albedo",              TextureType::ALBEDO,              vk::Format::eR8G8B8A8Srgb},
		{"default_normal",              TextureType::NORMAL,              vk::Format::eR8G8B8A8Unorm},
		{"default_metallic_roughness",  TextureType::METALLIC_ROUGHNESS,  vk::Format::eR8G8B8A8Unorm},
		{"default_mr_unit",             TextureType::METALLIC_ROUGHNESS,  vk::Format::eR8G8B8A8Unorm},
		{"default_occlusion",           TextureType::OCCLUSION,           vk::Format::eR8G8B8A8Unorm},
		{"default_emissive",            TextureType::EMISSIVE,            vk::Format::eR8G8B8A8Unorm},
		{"default_specular",            TextureType::SPECULAR,            vk::Format::eR8G8B8A8Unorm},
		{"default_specular_color",      TextureType::SPECULAR_COLOR,      vk::Format::eR8G8B8A8Srgb},
		{"default_particle",            TextureType::EMISSIVE,            vk::Format::eR8G8B8A8Unorm},
	};
	DecodedTexture decoded;
	const DefaultEntry* def = nullptr;
	for (const auto& e : defaults)
		if (m_resource_id == e.id) { def = &e; break; }

	if (def) {
		decoded.resource_id = m_resource_id;
		decoded.type = def->type;
		decoded.format = def->format;
		decoded.mip_levels = 1;
		decoded.array_layers = 1;
		decoded.width = 4;
		decoded.height = 4;
		if (m_resource_id == "default_mr_unit") {
			decoded.pixels.assign(static_cast<size_t>(decoded.width) * decoded.height * 4, 255);
		} else if (m_resource_id == "default_particle") {
			// Soft radial gradient
			decoded.width = 64;
			decoded.height = 64;
			decoded.pixels.resize(decoded.width * decoded.height * 4);
			const float cx = static_cast<float>(decoded.width - 1) * 0.5f;
			const float cy = static_cast<float>(decoded.height - 1) * 0.5f;
			const float r_max = cx;
			for (uint32_t y = 0; y < decoded.height; ++y) {
				for (uint32_t x = 0; x < decoded.width; ++x) {
					float dx = static_cast<float>(x) - cx;
					float dy = static_cast<float>(y) - cy;
					float d = std::sqrt(dx * dx + dy * dy) / r_max;
					float t = std::clamp(1.0f - d, 0.0f, 1.0f);
					float a = t * t * (3.0f - 2.0f * t); // smoothstep
					size_t i = (y * decoded.width + x) * 4;
					decoded.pixels[i + 0] = 255;
					decoded.pixels[i + 1] = 255;
					decoded.pixels[i + 2] = 255;
					decoded.pixels[i + 3] = static_cast<uint8_t>(a * 255.0f);
				}
			}
		} else {
			stbi_uc* pixels = generateDefaultTexture(static_cast<int>(decoded.width), static_cast<int>(decoded.height), def->type);
			const size_t byte_count = static_cast<size_t>(decoded.width) * static_cast<size_t>(decoded.height) * 4u;
			decoded.pixels.assign(pixels, pixels + byte_count);
			free(pixels);
		}
	} else {
		// Resource id is "<path>|srgb" or "<path>|linear" (see makeResourceKey).
		// Pick a representative TextureType so decode() applies the right format hint.
		std::string path_str = m_resource_id;
		TextureType type = TextureType::ALBEDO;  // srgb bucket
		if (size_t pipe = path_str.find('|'); pipe != std::string::npos) {
			std::string suffix = path_str.substr(pipe + 1);
			path_str = path_str.substr(0, pipe);
			if (suffix == "linear")
				type = TextureType::NORMAL;  // linear bucket
		}
		GpuCaps caps{m_ve_device.supportsBC(), m_ve_device.supportsASTC()};
		decoded = decode(std::filesystem::path(path_str), type, caps, nullptr);
		if (decoded.pixels.empty()) {
			VE_LOGE("Failed to load texture: " << path_str);
			return false;
		}
	}

	SyncUploadScope scope(m_ve_device, std::max<vk::DeviceSize>(decoded.pixels.size(), 4096));
	if (!recordDecodedTexture(decoded, scope.ctx))
		return false;
	createTextureSampler();
	return true;
}

void VeTexture::doUnload() {
	m_texture_sampler.reset();
	m_texture_image.reset();
	m_width = 0;
	m_height = 0;
}

void VeTexture::emitUnloadingEvent(EventBus& bus) {
	ResourceUnloadingEvent<VeTexture> ev{};
	ev.resource = this;
	bus.emitImmediate(ev);
}

const vk::raii::Sampler& VeTexture::getSampler() const {
	assert(m_texture_sampler && "VeTexture not loaded");
	return *m_texture_sampler;
}

bool VeTexture::recordDecodedTexture(const DecodedTexture& decoded, UploadContext& ctx) {
	if (decoded.pixels.empty() || decoded.width == 0 || decoded.height == 0) {
		VE_LOGE("recordDecodedTexture: empty payload for " << m_resource_id);
		return false;
	}

	m_width = decoded.width;
	m_height = decoded.height;
	const uint32_t mip_levels = std::max(1u, decoded.mip_levels);
	const uint32_t array_layers = std::max(1u, decoded.array_layers);
	const bool use_mipmap_copy = (mip_levels > 1) || !decoded.mip_extents.empty();

	const size_t total_size = decoded.pixels.size();
	auto src = ctx.arena.write(decoded.pixels.data(), total_size);

	// Per-mip offsets are relative to the start of decoded.pixels
	std::vector<vk::DeviceSize> buffer_offsets;
	std::vector<vk::Extent3D> extents;
	if (use_mipmap_copy) {
		buffer_offsets.reserve(decoded.mip_offsets.size());
		for (vk::DeviceSize off : decoded.mip_offsets)
			buffer_offsets.push_back(off + src.offset);
		extents = decoded.mip_extents;
	} else {
		buffer_offsets.push_back(src.offset);
		extents.push_back({ m_width, m_height, 1 });
	}

	m_texture_image = std::make_unique<ve::VeImage>(
		m_ve_device, m_width, m_height,
		vk::SampleCountFlagBits::e1, decoded.format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		decoded.is_cubemap, array_layers, mip_levels);

	m_texture_image->transitionImageLayout(
		ctx.transfer_cmd,
		vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		{}, vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eTransfer);
	ctx.transfer_has_work = true;

	if (use_mipmap_copy) {
		VeDevice::copyBufferToImageWithMipmaps(
			ctx.transfer_cmd, src.buffer, m_texture_image->getImage(),
			array_layers, mip_levels, buffer_offsets, extents);
	} else {
		// Single-mip path: emit one BufferImageCopy with the arena offset.
		vk::BufferImageCopy copy_region{
			.bufferOffset = src.offset,
			.bufferRowLength = 0,
			.bufferImageHeight = 0,
			.imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, array_layers },
			.imageOffset = { 0, 0, 0 },
			.imageExtent = { m_width, m_height, 1 }
		};
		ctx.transfer_cmd.copyBufferToImage(src.buffer, m_texture_image->getImage(),
			vk::ImageLayout::eTransferDstOptimal, copy_region);
	}
	ctx.transfer_has_work = true;

	m_texture_image->transitionImageLayout(
		ctx.graphics_cmd,
		vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader);
	ctx.graphics_has_work = true;

	m_texture_image->setDebugName(m_resource_id.c_str());

	ctx.bytes_in_flight += total_size;
	return true;
}

// Sets max anisotropy to the maximum value supported by the device or 16, whichever is lower
void VeTexture::createTextureSampler() {
	auto max_anisotropy = std::min(16.0f, m_ve_device.getDeviceProperties().limits.maxSamplerAnisotropy);

	// Disable anisotropy on Arm GPUs for better performance
	// Arm GPUs have less efficient descriptors with anisotropy enabled
	// according to vulkan validation layers
#if defined(__arm64__) || defined(__aarch64__) || (defined(__APPLE__) && defined(__ARM_ARCH))
	bool enable_anisotropy = false;
#else
	bool enable_anisotropy = true;
#endif

	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eLinear,
		.addressModeU = vk::SamplerAddressMode::eRepeat,
		.addressModeV = vk::SamplerAddressMode::eRepeat,
		.addressModeW = vk::SamplerAddressMode::eRepeat,
		.mipLodBias = 0.0f,
		.anisotropyEnable = enable_anisotropy ? vk::True : vk::False,
		.maxAnisotropy = enable_anisotropy ? max_anisotropy : 1.0f,
		.compareEnable = vk::False,
		.compareOp = vk::CompareOp::eAlways,
		.minLod = 0.0f,
		.maxLod = VK_LOD_CLAMP_NONE,
		.borderColor = vk::BorderColor::eIntTransparentBlack,
		.unnormalizedCoordinates = vk::False
	};
	m_texture_sampler.emplace(m_ve_device.getDevice(), sampler_info);
}

vk::raii::Sampler VeTexture::createDepthCompareSampler(VeDevice& device) {
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eLinear,
		.addressModeU = vk::SamplerAddressMode::eClampToBorder,
		.addressModeV = vk::SamplerAddressMode::eClampToBorder,
		.addressModeW = vk::SamplerAddressMode::eClampToBorder,
		.mipLodBias = 0.0f,
		.anisotropyEnable = vk::False,
		.maxAnisotropy = 1.0f,
		.compareEnable = vk::True,
		.compareOp = vk::CompareOp::eLess,
		.minLod = 0.0f,
		.maxLod = VK_LOD_CLAMP_NONE,
		.borderColor = vk::BorderColor::eFloatOpaqueWhite,  // outside shadow map = lit
		.unnormalizedCoordinates = vk::False
	};
	return vk::raii::Sampler(device.getDevice(), sampler_info);
}

vk::raii::Sampler VeTexture::createShadowRawSampler(VeDevice& device) {
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eLinear,
		.addressModeU = vk::SamplerAddressMode::eClampToBorder,
		.addressModeV = vk::SamplerAddressMode::eClampToBorder,
		.addressModeW = vk::SamplerAddressMode::eClampToBorder,
		.mipLodBias = 0.0f,
		.anisotropyEnable = vk::False,
		.maxAnisotropy = 1.0f,
		.compareEnable = vk::False,
		.minLod = 0.0f,
		.maxLod = VK_LOD_CLAMP_NONE,
		.borderColor = vk::BorderColor::eFloatOpaqueWhite,
		.unnormalizedCoordinates = vk::False
	};
	return vk::raii::Sampler(device.getDevice(), sampler_info);
}

// Generates a simple 4-channel RGBA texture based on the type.
// Callers must free the returned pixel data after use.
stbi_uc* VeTexture::generateDefaultTexture(int width, int height, TextureType type) {
	assert(width > 0 && height > 0 && "Width and height must be greater than 0");
    stbi_uc* pixels = (stbi_uc*)malloc(static_cast<size_t>(width * height * 4));

	size_t dimension = static_cast<size_t>(width * height);
    for (size_t i = 0; i < dimension; i++) {
        switch (type) {
            case TextureType::ALBEDO:
                // White (1, 1, 1, 1)
                pixels[i * 4 + 0] = 255;
                pixels[i * 4 + 1] = 255;
                pixels[i * 4 + 2] = 255;
                pixels[i * 4 + 3] = 255;
                break;

            case TextureType::NORMAL:
                // Flat normal (0.5, 0.5, 1.0) or (128, 128, 255) in RGB
                pixels[i * 4 + 0] = 128;
                pixels[i * 4 + 1] = 128;
                pixels[i * 4 + 2] = 255;
                pixels[i * 4 + 3] = 255;
                break;

            case TextureType::METALLIC_ROUGHNESS:
                // Non-metallic (0), rough (1) or (0, 255, 0, 255)
                // Metallic in B, Roughness in G (glTF convention)
                pixels[i * 4 + 0] = 0;
                pixels[i * 4 + 1] = 255;
                pixels[i * 4 + 2] = 0;
                pixels[i * 4 + 3] = 255;
                break;

            case TextureType::OCCLUSION:
                // White: no darkening of ambient
                pixels[i * 4 + 0] = 255;
                pixels[i * 4 + 1] = 255;
                pixels[i * 4 + 2] = 255;
                pixels[i * 4 + 3] = 255;
                break;

            case TextureType::EMISSIVE:
                // White: emissive factor alone controls emission (glTF spec)
            case TextureType::SPECULAR:
                // KHR_materials_specular default
            case TextureType::SPECULAR_COLOR:
                // KHR_materials_specular default
                pixels[i * 4 + 0] = 255;
                pixels[i * 4 + 1] = 255;
                pixels[i * 4 + 2] = 255;
                pixels[i * 4 + 3] = 255;
                break;
        }
    }
    return pixels;
}


vk::DescriptorImageInfo VeTexture::getDescriptorInfo() const {
	assert(isLoaded() && m_texture_sampler && m_texture_image && "VeTexture not loaded");
	vk::DescriptorImageInfo image_info{
		.sampler = *m_texture_sampler,
		.imageView = *m_texture_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};
	return image_info;
}

}