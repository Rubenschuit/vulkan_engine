#include "pch.hpp"
#include "resources/ve_texture.hpp"
#include "resources/loaded_asset_data.hpp"
#include "vulkan/ve_buffer.hpp"

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
}

// Static cache for embedded images from .glb files.
// Not thread-safe.
static std::unordered_map<std::string, ve::EmbeddedImageData> s_embedded_cache;

namespace ve {

VeTexture::VeTexture(VeDevice& ve_device, const std::string& resource_id)
	: Resource(resource_id), m_ve_device(ve_device) {
}

VeTexture::VeTexture(VeDevice& ve_device, uint32_t width, uint32_t height, TextureType type)
	: Resource("default"), m_ve_device(ve_device), m_width(width), m_height(height) {
	stbi_uc* pixels = generateDefaultTexture(static_cast<int>(width), static_cast<int>(height), type);
	vk::Format format = (type == TextureType::ALBEDO) ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
	createTextureImageFromPixels(width, height, pixels, format);
	free(pixels);
	createTextureSampler();
	setLoaded(true);
}

std::shared_ptr<VeTexture> VeTexture::createDefault(VeDevice& device, TextureType type) {
	return std::make_shared<VeTexture>(device, 4, 4, type);
}

ResourceHandle<VeTexture> VeTexture::loadOrDefault(VeResourceManager& resource_manager, const std::filesystem::path& path, TextureType fallback_type) {
	std::string key = path.lexically_normal().generic_string();
	auto fn = path.filename().string();
	bool is_default = (fn == "default_albedo.png" || fn == "default_normal.png" || fn == "default_metallic_roughness.png" ||
	                   fn == "default_occlusion.png" || fn == "default_emissive.png" || fn == "default_mr_unit.png" ||
	                   fn == "white.png" || fn == "black.png");
	if (is_default || (!std::filesystem::exists(path) && !hasEmbedded(key))) {
		if (fn == "default_mr_unit.png")
			return resource_manager.load<VeTexture>("default_mr_unit");
		const char* default_id = "default_metallic_roughness";
		if (fallback_type == TextureType::ALBEDO) default_id = "default_albedo";
		else if (fallback_type == TextureType::NORMAL) default_id = "default_normal";
		else if (fallback_type == TextureType::OCCLUSION) default_id = "default_occlusion";
		else if (fallback_type == TextureType::EMISSIVE) default_id = "default_emissive";
		return resource_manager.load<VeTexture>(default_id);
	}
	const char* suffix = "|mr";
	if (fallback_type == TextureType::ALBEDO) suffix = "|albedo";
	else if (fallback_type == TextureType::NORMAL) suffix = "|normal";
	else if (fallback_type == TextureType::OCCLUSION) suffix = "|occlusion";
	else if (fallback_type == TextureType::EMISSIVE) suffix = "|emissive";
	return resource_manager.load<VeTexture>(key + suffix);
}

void VeTexture::registerEmbedded(const std::string& key, EmbeddedImageData data) {
	s_embedded_cache[key] = std::move(data);
}

bool VeTexture::hasEmbedded(const std::string& key) {
	return s_embedded_cache.find(key) != s_embedded_cache.end();
}

void VeTexture::clearEmbeddedCache() {
	s_embedded_cache.clear();
}

VeTexture::~VeTexture() {
	unload();
}

bool VeTexture::doLoad() {
	// Default placeholder textures
	static const struct {
		const char* id;
		TextureType type;
		vk::Format format;
	} defaults[] = {
		{"default_albedo",              TextureType::ALBEDO,              vk::Format::eR8G8B8A8Srgb},
		{"default_normal",              TextureType::NORMAL,              vk::Format::eR8G8B8A8Unorm},
		{"default_metallic_roughness",  TextureType::METALLIC_ROUGHNESS,  vk::Format::eR8G8B8A8Unorm},
		{"default_mr_unit",             TextureType::METALLIC_ROUGHNESS,  vk::Format::eR8G8B8A8Unorm},
		{"default_occlusion",           TextureType::OCCLUSION,           vk::Format::eR8G8B8A8Unorm},
		{"default_emissive",            TextureType::EMISSIVE,            vk::Format::eR8G8B8A8Unorm},
	};
	for (const auto& def : defaults) {
		if (m_resource_id != def.id)
			continue;
		stbi_uc* pixels;
		if (m_resource_id == "default_mr_unit") {
			// Full metallic (B=255) and full roughness (G=255) so MaterialFactors alone control the values
			pixels = (stbi_uc*)malloc(4 * 4 * 4);
			for (size_t i = 0; i < 16; i++) {
				pixels[i * 4 + 0] = 0;
				pixels[i * 4 + 1] = 255;  // roughness = 1.0
				pixels[i * 4 + 2] = 255;  // metallic = 1.0
				pixels[i * 4 + 3] = 255;
			}
		} else {
			pixels = generateDefaultTexture(4, 4, def.type);
		}
		createTextureImageFromPixels(4, 4, pixels, def.format);
		free(pixels);
		createTextureSampler();
		return true;
	}

	// Path with format suffix (e.g. "path/to/tex.png|albedo") for explicit format
	std::string path_str = m_resource_id;
	vk::Format format = vk::Format::eR8G8B8A8Srgb;  // default
	size_t pipe = path_str.find('|');
	if (pipe != std::string::npos) {
		std::string suffix = path_str.substr(pipe + 1);
		path_str = path_str.substr(0, pipe);
		if (suffix == "normal" || suffix == "mr" || suffix == "occlusion" || suffix == "emissive")
			format = vk::Format::eR8G8B8A8Unorm;
	}

	// Check embedded image cache (glb support)
	auto emb_it = s_embedded_cache.find(path_str);
	if (emb_it != s_embedded_cache.end()) {
		auto& data = emb_it->second;
		bool ok;
		if (data.is_ktx) {
			ok = createTextureImageFromKtxMemory(data.pixels.data(), data.pixels.size(), format);
		} else {
			m_width = data.width;
			m_height = data.height;
			createTextureImageFromPixels(data.width, data.height, data.pixels.data(), format);
			ok = true;
		}
		if (ok)
			createTextureSampler();
		// Don't erase — the same image may be referenced by multiple texture types
		// (e.g. ORM shared between |mr and |occlusion). clearEmbeddedCache() handles cleanup.
		return ok;
	}

	std::filesystem::path path(path_str);
	if (path.extension() == ".ktx" || path.extension() == ".ktx2") {
		if (!createTextureImage(path, format)) {
			VE_LOGE("Failed to load KTX texture: " << path.string());
			return false;
		}
	} else {
		if (!createTextureImageSTB(path, format)) {
			VE_LOGE("Failed to load image: " << path.string());
			return false;
		}
	}
	createTextureSampler();
	return true;
}

void VeTexture::doUnload() {
	m_texture_sampler.reset();
	m_texture_image.reset();
	m_width = 0;
	m_height = 0;
}

const vk::raii::Sampler& VeTexture::getSampler() const {
	assert(m_texture_sampler && "VeTexture not loaded");
	return *m_texture_sampler;
}

// Loads a texture from a .ktx or .ktx2 file and creates Vulkan image resources.
bool VeTexture::createTextureImage(const std::filesystem::path& texture_path, vk::Format format_hint) {
	ktxTexture* k_texture;
	KTX_error_code result = ktxTexture_CreateFromNamedFile(
		texture_path.string().c_str(),
		KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
		&k_texture);

	if (result != KTX_SUCCESS) {
		VE_LOGE("KTX load failed: " << texture_path << " (error " << static_cast<int>(result) << ")");
		return false;
	}

	return uploadKtxTexture(k_texture, format_hint);
}

// Loads a KTX/KTX2 texture from raw bytes in memory (for .glb embedded images).
bool VeTexture::createTextureImageFromKtxMemory(const uint8_t* mem_data, size_t mem_size, vk::Format format_hint) {
	ktxTexture* k_texture;
	KTX_error_code result = ktxTexture_CreateFromMemory(
		mem_data, mem_size,
		KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
		&k_texture);

	if (result != KTX_SUCCESS) {
		VE_LOGE("KTX load from memory failed (error " << static_cast<int>(result) << ")");
		return false;
	}

	return uploadKtxTexture(k_texture, format_hint);
}

// Shared KTX upload: transcodes, stages, and uploads a ktxTexture to GPU.
// Takes ownership of k_texture and destroys it before returning.
bool VeTexture::uploadKtxTexture(void* k_texture_ptr, vk::Format format_hint) {
	auto* k_texture = static_cast<ktxTexture*>(k_texture_ptr);
	bool is_cubemap = k_texture->isCubemap;
	m_width = k_texture->baseWidth;
	m_height = k_texture->baseHeight;

	if (m_width == 0 || m_height == 0) {
		VE_LOGE("KTX texture has invalid dimensions: " << m_width << "x" << m_height);
		ktxTexture_Destroy(k_texture);
		return false;
	}

	// Handle transcoding for compressed KTX2 textures (BasisLZ / UASTC)
	if (k_texture->classId == ktxTexture2_c) {
		ktxTexture2* ktx2 = reinterpret_cast<ktxTexture2*>(k_texture);
		if (ktxTexture2_NeedsTranscoding(ktx2)) {
			ktx_transcode_fmt_e target = KTX_TTF_RGBA32;
			if (m_ve_device.supportsBC())
				target = KTX_TTF_BC7_RGBA;
			else if (m_ve_device.supportsASTC())
				target = KTX_TTF_ASTC_4x4_RGBA;
			KTX_error_code result = ktxTexture2_TranscodeBasis(ktx2, target, 0);
			if (result != KTX_SUCCESS) {
				VE_LOGE("KTX2 transcoding failed (error " << static_cast<int>(result) << ")");
				ktxTexture_Destroy(k_texture);
				return false;
			}
		}
	}

	uint32_t num_levels = k_texture->numLevels;
	ktx_uint8_t* data = ktxTexture_GetData(k_texture);

	vk::Format texture_format;
	if (k_texture->classId == ktxTexture2_c) {
		auto* ktx2 = reinterpret_cast<ktxTexture2*>(k_texture);
		texture_format = static_cast<vk::Format>(ktx2->vkFormat);
		if (texture_format == vk::Format::eUndefined)
			texture_format = vk::Format::eR8G8B8A8Unorm;
	} else if (k_texture->classId == ktxTexture1_c) {
		texture_format = static_cast<vk::Format>(ktxTexture1_GetVkFormat(reinterpret_cast<ktxTexture1*>(k_texture)));
		if (texture_format == vk::Format::eUndefined)
			texture_format = vk::Format::eR8G8B8A8Unorm;
	} else {
		texture_format = vk::Format::eR8G8B8A8Unorm;
	}

	// Apply format_hint (sRGB vs linear) to the transcoded format.
	// For uncompressed 8-bit: override directly. For block-compressed: convert to sRGB/Unorm variant.
	bool is_uncompressed_8bit = (texture_format == vk::Format::eR8G8B8Srgb || texture_format == vk::Format::eR8G8B8Unorm ||
	                            texture_format == vk::Format::eR8G8B8A8Srgb || texture_format == vk::Format::eR8G8B8A8Unorm);
	bool has_format_hint = (format_hint == vk::Format::eR8G8B8A8Srgb || format_hint == vk::Format::eR8G8B8A8Unorm);
	if (has_format_hint && is_uncompressed_8bit)
		texture_format = format_hint;
	else if (has_format_hint && isBlockCompressed(texture_format)) {
		bool want_srgb = (format_hint == vk::Format::eR8G8B8A8Srgb);
		texture_format = want_srgb ? toSrgbBC(texture_format) : toUnormBC(texture_format);
	}

	const uint32_t array_layers = is_cubemap ? 6 : 1;
	const uint32_t effective_levels = num_levels;
	const bool use_mipmap_copy = (effective_levels > 1);

	ktx_size_t total_size = 0;
	std::vector<vk::DeviceSize> buffer_offsets;
	std::vector<vk::Extent3D> extents;
	if (use_mipmap_copy) {
		for (uint32_t level = 0; level < effective_levels; level++) {
			ktx_size_t offset = 0;
			ktxTexture_GetImageOffset(k_texture, level, 0, 0, &offset);
			buffer_offsets.push_back(offset);
			uint32_t w = std::max(1u, m_width >> level);
			uint32_t h = std::max(1u, m_height >> level);
			extents.push_back({ w, h, 1 });
		}
		total_size = ktxTexture_GetDataSize(k_texture);
	} else {
		ktx_size_t image_size = ktxTexture_GetImageSize(k_texture, 0);
		total_size = image_size * array_layers;
		buffer_offsets.push_back(0);
		extents.push_back({ m_width, m_height, 1 });
	}

	ve::VeBuffer staging_buffer(
		m_ve_device, total_size, 1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	staging_buffer.map();
	staging_buffer.writeToBuffer(data, static_cast<size_t>(total_size));

	m_texture_image = std::make_unique<ve::VeImage>(
		m_ve_device, m_width, m_height,
		vk::SampleCountFlagBits::e1, texture_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		is_cubemap, array_layers, effective_levels);

	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		{}, vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eTransfer);

	if (use_mipmap_copy) {
		m_ve_device.copyBufferToImageWithMipmaps(
			staging_buffer.getBuffer(), m_texture_image->getImage(),
			array_layers, effective_levels, buffer_offsets, extents);
	} else {
		m_ve_device.copyBufferToImage(
			staging_buffer.getBuffer(), m_texture_image->getImage(),
			m_width, m_height, array_layers);
	}

	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader);
	m_texture_image->setDebugName(m_resource_id.c_str());

	ktxTexture_Destroy(k_texture);
	return true;
}

void VeTexture::createTextureImageFromPixels(uint32_t width, uint32_t height, const stbi_uc* pixels, vk::Format format) {
	uint32_t pixel_count = static_cast<uint32_t>(width * height);
	ve::VeBuffer staging_buffer(
		m_ve_device,
		4,
		pixel_count,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(pixels);
	m_texture_image = std::make_unique<ve::VeImage>(
		m_ve_device,
		static_cast<uint32_t>(width),
		static_cast<uint32_t>(height),
		vk::SampleCountFlagBits::e1,
		format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false,
		1
	);
	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eTransferDstOptimal,
		{},
		vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eTransfer);
	m_ve_device.copyBufferToImage(
		staging_buffer.getBuffer(),
		m_texture_image->getImage(),
		static_cast<uint32_t>(width),
		static_cast<uint32_t>(height));
	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::PipelineStageFlagBits2::eFragmentShader);
	m_texture_image->setDebugName(m_resource_id.c_str());
}

// Loads a texture from a file using stb_image.
bool VeTexture::createTextureImageSTB(const std::filesystem::path& texture_path, vk::Format format) {
	int channels, width, height;
	stbi_uc* pixels = stbi_load(texture_path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);

	if (!pixels) {
		const char* reason = stbi_failure_reason();
		VE_LOGE("STB load failed: " << texture_path << " (" << (reason ? reason : "unknown") << ")");
		return false;
	}

	(void)channels;
	m_width = static_cast<uint32_t>(width);
	m_height = static_cast<uint32_t>(height);
	VE_LOGD("Texture width x height: " << m_width << " x " << m_height);

	createTextureImageFromPixels(m_width, m_height, pixels, format);
	stbi_image_free(pixels);
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