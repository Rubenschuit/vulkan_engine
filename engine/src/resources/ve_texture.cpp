#include "pch.hpp"
#include "resources/ve_texture.hpp"
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
	                   fn == "default_occlusion.png" || fn == "default_emissive.png" || fn == "white.png" || fn == "black.png");
	if (is_default || !std::filesystem::exists(path)) {
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

VeTexture::~VeTexture() {
	unload();
}

bool VeTexture::doLoad() {
	// Default placeholder textures (create from pixels)
	if (m_resource_id == "default_albedo") {
		stbi_uc* pixels = generateDefaultTexture(4, 4, TextureType::ALBEDO);
		createTextureImageFromPixels(4, 4, pixels, vk::Format::eR8G8B8A8Srgb);
		free(pixels);
		createTextureSampler();
		return true;
	}
	if (m_resource_id == "default_normal") {
		stbi_uc* pixels = generateDefaultTexture(4, 4, TextureType::NORMAL);
		createTextureImageFromPixels(4, 4, pixels, vk::Format::eR8G8B8A8Unorm);
		free(pixels);
		createTextureSampler();
		return true;
	}
	if (m_resource_id == "default_metallic_roughness") {
		stbi_uc* pixels = generateDefaultTexture(4, 4, TextureType::METALLIC_ROUGHNESS);
		createTextureImageFromPixels(4, 4, pixels, vk::Format::eR8G8B8A8Unorm);
		free(pixels);
		createTextureSampler();
		return true;
	}
	if (m_resource_id == "default_occlusion") {
		stbi_uc* pixels = generateDefaultTexture(4, 4, TextureType::OCCLUSION);
		createTextureImageFromPixels(4, 4, pixels, vk::Format::eR8G8B8A8Unorm);
		free(pixels);
		createTextureSampler();
		return true;
	}
	if (m_resource_id == "default_emissive") {
		stbi_uc* pixels = generateDefaultTexture(4, 4, TextureType::EMISSIVE);
		createTextureImageFromPixels(4, 4, pixels, vk::Format::eR8G8B8A8Unorm);
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

// TODO: generate mipmaps if not present
// Loads a texture from a .ktx or .ktx2 file and creates a Vulkan image resources.
// Also works for cubemaps.
// format_hint: preferred format when converting R8G8B8->RGBA.
// Normals for example should use Unorm (linear).
bool VeTexture::createTextureImage(const std::filesystem::path& texture_path, vk::Format format_hint) {
	//VE_LOGD("Loading texture from " << texture_path);

	ktxTexture* k_texture;
    KTX_error_code result = ktxTexture_CreateFromNamedFile(
        texture_path.string().c_str(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &k_texture);

	if (result != KTX_SUCCESS) {
		VE_LOGE("KTX load failed: " << texture_path << " (error " << static_cast<int>(result) << ")");
		return false;
	}

	//VE_LOGD("Texture loaded successfully");

	// Get texture dimensions and data
	bool is_cubemap = k_texture->isCubemap;
    m_width = k_texture->baseWidth;
    m_height = k_texture->baseHeight;

	if (m_width == 0 || m_height == 0) {
		VE_LOGE("KTX texture has invalid dimensions: " << m_width << "x" << m_height << " (" << texture_path << ")");
		ktxTexture_Destroy(k_texture);
		return false;
	}

    // Handle transcoding for compressed KTX2 textures (BasisLZ / UASTC)
    // Transcode to GPU-compressed formats when supported to reduce VRAM usage ~4x
    if (k_texture->classId == ktxTexture2_c) {
        ktxTexture2* ktx2 = reinterpret_cast<ktxTexture2*>(k_texture);
        if (ktxTexture2_NeedsTranscoding(ktx2)) {
            ktx_transcode_fmt_e target = KTX_TTF_RGBA32;
            if (m_ve_device.supportsBC()) {
                target = KTX_TTF_BC7_RGBA;
            } else if (m_ve_device.supportsASTC()) {
                target = KTX_TTF_ASTC_4x4_RGBA;
            }
            result = ktxTexture2_TranscodeBasis(ktx2, target, 0);
            if (result != KTX_SUCCESS) {
                VE_LOGE("KTX2 transcoding failed: " << texture_path << " (error " << static_cast<int>(result) << ")");
                ktxTexture_Destroy(k_texture);
                return false;
            }
        }
    }

	uint32_t num_levels = k_texture->numLevels; // number of mip levels
	ktx_uint8_t* data = ktxTexture_GetData(k_texture);
	//VE_LOGD("Texture dimensions: " << m_width << "x" << m_height);
	//VE_LOGD("Mip levels: " << num_levels);

	vk::Format texture_format;
	if (k_texture->classId == ktxTexture2_c) {
		auto* ktx2 = reinterpret_cast<ktxTexture2*>(k_texture);
		texture_format = static_cast<vk::Format>(ktx2->vkFormat);
		//VE_LOGD("Texture format: " << vk::to_string(texture_format));
		if (texture_format == vk::Format::eUndefined)
			texture_format = vk::Format::eR8G8B8A8Unorm;
	} else if (k_texture->classId == ktxTexture1_c) {
		texture_format = static_cast<vk::Format>(ktxTexture1_GetVkFormat(reinterpret_cast<ktxTexture1*>(k_texture)));
		if (texture_format == vk::Format::eUndefined) {
			VE_LOGW("KTX1 format unknown for " << texture_path << ", assuming R8G8B8A8Unorm");
			texture_format = vk::Format::eR8G8B8A8Unorm;
		}
		VE_LOGD("Texture format (KTX1): " << vk::to_string(texture_format));
	} else {
		texture_format = vk::Format::eR8G8B8A8Unorm;
	}

	// Apply format_hint (sRGB vs linear) to the transcoded format.
	// For uncompressed 8-bit: override directly. For block-compressed: convert to sRGB/Unorm variant.
	bool is_uncompressed_8bit = (texture_format == vk::Format::eR8G8B8Srgb || texture_format == vk::Format::eR8G8B8Unorm ||
	                            texture_format == vk::Format::eR8G8B8A8Srgb || texture_format == vk::Format::eR8G8B8A8Unorm);
	bool has_format_hint = (format_hint == vk::Format::eR8G8B8A8Srgb || format_hint == vk::Format::eR8G8B8A8Unorm);
	if (has_format_hint && is_uncompressed_8bit) {
		texture_format = format_hint;
	} else if (has_format_hint && isBlockCompressed(texture_format)) {
		bool want_srgb = (format_hint == vk::Format::eR8G8B8A8Srgb);
		texture_format = want_srgb ? toSrgbBC(texture_format) : toUnormBC(texture_format);
	}



	// Compute total size and copy regions. For cubemaps with mipmaps we use single-level path.
	// Cubemap mipmap copy is not implemented - only level 0 is copied, so force num_levels=1 to avoid
	// sampling uninitialized mip levels (which causes pink).
	const uint32_t array_layers = is_cubemap ? 6 : 1;
	const uint32_t effective_levels = (is_cubemap && num_levels > 1) ? 1u : num_levels;
	const bool use_mipmap_copy = (effective_levels > 1 && !is_cubemap);

	ktx_size_t total_size = 0;
	std::vector<vk::DeviceSize> buffer_offsets;
	std::vector<vk::Extent3D> extents;
	if (use_mipmap_copy) {
		for (uint32_t level = 0; level < effective_levels; level++) {
			ktx_size_t offset = 0;
			ktxTexture_GetImageOffset(k_texture, level, 0, 0, &offset);
			buffer_offsets.push_back(offset);
			ktx_size_t level_size = ktxTexture_GetImageSize(k_texture, level);
			total_size += level_size;
			uint32_t w = std::max(1u, m_width >> level);
			uint32_t h = std::max(1u, m_height >> level);
			extents.push_back({ w, h, 1 });
		}
	} else {
		ktx_size_t image_size = ktxTexture_GetImageSize(k_texture, 0);
		total_size = image_size * array_layers;
		buffer_offsets.push_back(0);
		extents.push_back({ m_width, m_height, 1 });
	}
	//VE_LOGD("Total size: " << total_size);

	// Create staging buffer and copy data
	ve::VeBuffer staging_buffer(
		m_ve_device,
		total_size,
		1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(data, static_cast<size_t>(total_size));
	//VE_LOGD("Copied texture data to staging buffer");

	// Create image with mip levels
	m_texture_image = std::make_unique<ve::VeImage>(
		m_ve_device,
		static_cast<uint32_t>(m_width),
		static_cast<uint32_t>(m_height),
		vk::SampleCountFlagBits::e1,
		texture_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		is_cubemap,
		array_layers,
		effective_levels
	);

	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eTransferDstOptimal,
		{},
		vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eTransfer);

	if (use_mipmap_copy) {
		m_ve_device.copyBufferToImageWithMipmaps(
			staging_buffer.getBuffer(),
			m_texture_image->getImage(),
			array_layers,
			effective_levels,
			buffer_offsets,
			extents
		);
	} else {
		m_ve_device.copyBufferToImage(
			staging_buffer.getBuffer(),
			m_texture_image->getImage(),
			m_width,
			m_height,
			array_layers
		);
	}

	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::PipelineStageFlagBits2::eFragmentShader);

	//VE_LOGD("Texture created successfully: " << m_width << "x" << m_height << " format=" << vk::to_string(texture_format));
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

	// Create a local scope staging buffer
	ve::VeBuffer staging_buffer(
		m_ve_device,
		4,                                        // instance size
		static_cast<uint32_t>(width * height),    // instance count
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);

	// Copy image data to staging buffer
	staging_buffer.map();
	staging_buffer.writeToBuffer(pixels);
	// unmap is called in the destructor of VeBuffer
	stbi_image_free(pixels);

	// Create image
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
	// Next we execute synchronously 3 single-time command buffers:
	// TODO: consider combining these into one command buffer

	//transition image to be optimal for receiving data from buffer
	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eTransferDstOptimal,
		{},
		vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eTransfer);

	// Copy data from staging buffer to texture image
	m_ve_device.copyBufferToImage(
		staging_buffer.getBuffer(),
		m_texture_image->getImage(),
		static_cast<uint32_t>(width),
		static_cast<uint32_t>(height));

	// Transition image to be optimal for shader read access
	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::PipelineStageFlagBits2::eFragmentShader);
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
                // Flat normal (0.5, 0.5, 1.0) → (128, 128, 255) in RGB
                pixels[i * 4 + 0] = 128;
                pixels[i * 4 + 1] = 128;
                pixels[i * 4 + 2] = 255;
                pixels[i * 4 + 3] = 255;
                break;

            case TextureType::METALLIC_ROUGHNESS:
                // Non-metallic (0), rough (1) → (0, 255, 0, 255)
                // Metallic in B, Roughness in G (glTF convention)
                pixels[i * 4 + 0] = 0;
                pixels[i * 4 + 1] = 255;  // Roughness = 1
                pixels[i * 4 + 2] = 0;    // Metallic = 0
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