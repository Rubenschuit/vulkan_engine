#include "pch.hpp"
#include "ve_texture.hpp"
#include "ve_buffer.hpp"

#include <ktx.h>


namespace ve {

VeTexture::VeTexture(VeDevice& ve_device, const std::filesystem::path& texture_path) : m_ve_device(ve_device) {
	if (texture_path.extension() == ".ktx" || texture_path.extension() == ".ktx2")
		createTextureImage(texture_path);
	else
		createTextureImageSTB(texture_path);
	createTextureSampler();
}

// no ktx yet for arrays of textures
VeTexture::VeTexture(VeDevice& ve_device, const std::vector<std::filesystem::path>& texture_paths, vk::Format format) : m_ve_device(ve_device) {
	createTextureImageArraySTB(texture_paths, format);
	createTextureSampler();
}

VeTexture::~VeTexture(){}

// TODO: mip levels
// Loads a texture from a .ktx or .ktx2 file and creates a Vulkan image resources.
// Also works for cubemaps.
void VeTexture::createTextureImage(const std::filesystem::path& texture_path) {
	VE_LOGD("Loading texture from " << texture_path);

	ktxTexture* k_texture;
    KTX_error_code result = ktxTexture_CreateFromNamedFile(
        texture_path.string().c_str(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &k_texture);

	assert(result == KTX_SUCCESS && "Failed to load ktx texture image!");

	VE_LOGD("Texture loaded successfully");

	// Get texture dimensions and data
	bool is_cubemap = k_texture->isCubemap;
    m_width = k_texture->baseWidth;
    m_height = k_texture->baseHeight;

    // Handle transcoding for compressed KTX2 textures
    if (k_texture->classId == ktxTexture2_c) {
        ktxTexture2* ktx2 = reinterpret_cast<ktxTexture2*>(k_texture);
        if (ktxTexture2_NeedsTranscoding(ktx2)) {
            VE_LOGD("Texture is compressed, transcoding to RGBA32");
            ktxTexture2_TranscodeBasis(ktx2, KTX_TTF_RGBA32, 0);
        }
    }

    ktx_size_t image_size = ktxTexture_GetImageSize(k_texture, 0);
    ktx_uint8_t* data = ktxTexture_GetData(k_texture);

	VE_LOGD("Texture width: " << m_width);
	VE_LOGD("Texture height: " << m_height);
	VE_LOGD("Is cubemap: " << is_cubemap);
	VE_LOGD("Image size (one face): " << image_size);

	// For cubemaps, get total size (all 6 faces)
	ktx_size_t total_size = image_size * (is_cubemap ? 6 : 1);
	VE_LOGD("Total size: " << total_size);

	// Create a local scope staging buffer
	ve::VeBuffer staging_buffer(
		m_ve_device,
		image_size,                        // instance size (bytes per face)
		is_cubemap ? 6 : 1,                // instance count (number of faces)
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);

	// Copy image data to staging buffer
	staging_buffer.map();
	staging_buffer.writeToBuffer((void*)data, total_size);


	VE_LOGD("Copied texture data to staging buffer");

	vk::Format texture_format;
	// Check if the KTX texture has a format
	if (k_texture->classId == ktxTexture2_c) {
		// For KTX2 files, get the format after transcoding
		auto* ktx2 = reinterpret_cast<ktxTexture2*>(k_texture);
		texture_format = static_cast<vk::Format>(ktx2->vkFormat);
		VE_LOGD("Texture format: " << static_cast<uint32_t>(texture_format));
		if (texture_format == vk::Format::eUndefined) {
			texture_format = vk::Format::eR8G8B8A8Unorm;
			VE_LOGD("Texture format: eR8G8B8A8Unorm (ktx2 fallback)");
		}
	} else {
		// For KTX1 files or if we can't determine the format
		texture_format = vk::Format::eR8G8B8A8Unorm;
		VE_LOGD("Texture format: eR8G8B8A8Unorm (fallback)");
	}

	// Create image
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
		is_cubemap ? 6 : 1
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
		m_width,
		m_height,
		is_cubemap ? 6 : 1
	);

	// Transition image to be optimal for shader read access
	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::PipelineStageFlagBits2::eFragmentShader);

	VE_LOGD("Texture created successfully");
	ktxTexture_Destroy(k_texture);
}

// Loads a texture from a file using stb_image.
void VeTexture::createTextureImageSTB(const std::filesystem::path& texture_path) {
	// Load image from file using stb_image
	VE_LOGD("Loading texture from " << texture_path);
	int channels, width, height;
	stbi_uc* pixels = stbi_load(texture_path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);

	assert(pixels != nullptr && "Failed to load image");

	(void)channels;

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
	staging_buffer.writeToBuffer((void*)pixels);
	// unmap is called in the destructor of VeBuffer
	stbi_image_free(pixels);

	// Create image
	m_texture_image = std::make_unique<ve::VeImage>(
		m_ve_device,
		static_cast<uint32_t>(width),
		static_cast<uint32_t>(height),
		vk::SampleCountFlagBits::e1,
		vk::Format::eR8G8B8A8Srgb,
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
}

// Loads a array of textures from a file using stb_image.
void VeTexture::createTextureImageArraySTB(const std::vector<std::filesystem::path>& texture_paths, vk::Format format) {

	uint32_t layer_count = static_cast<uint32_t>(texture_paths.size());
	VE_LOGD("Loading texture array of " << layer_count << " textures");

	std::vector<stbi_uc*> pixels(layer_count);

	int channels, width = 0, height = 0;

	// First pass: find first real texture to get dimensions
	for (size_t i = 0; i < layer_count; i++) {
		if (texture_paths[i].filename() != "default_albedo.png" &&
		    texture_paths[i].filename() != "default_normal.png" &&
		    texture_paths[i].filename() != "default_metallic_roughness.png" &&
		    texture_paths[i].filename() != "white.png" &&
		    texture_paths[i].filename() != "black.png"
		) {
			stbi_uc* test_pixels = stbi_load(texture_paths[i].string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
			assert(test_pixels != nullptr && "Failed to load image to determine dimensions");
			stbi_image_free(test_pixels);
			VE_LOGD("Determined texture dimensions from first real texture: " << width << "x" << height);
			break;
		}
	}
	assert(width > 0 && height > 0 && "Could not determine texture dimensions - no real textures found");

	// load all textures (or generate defaults)
	int current_width = width;
	int current_height = height;
	for (size_t i = 0; i < layer_count; i++) {
		if (texture_paths[i].filename() == "default_albedo.png" || texture_paths[i].filename() == "white.png")
			pixels[i] = generateDefaultTexture(width, height, TextureType::ALBEDO);
		else if (texture_paths[i].filename() == "default_normal.png")
			pixels[i] = generateDefaultTexture(width, height, TextureType::NORMAL);
		else if (texture_paths[i].filename() == "default_metallic_roughness.png")
			pixels[i] = generateDefaultTexture(width, height, TextureType::METALLIC_ROUGHNESS);
		else
			pixels[i] = stbi_load(texture_paths[i].string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
		//VE_LOGD("Loaded texture " << texture_paths[i].filename() << " with dimensions " << width << "x" << height);
		assert(width == current_width && height == current_height && "All images in Texture Array must have the same width and height");
		assert(pixels[i] != nullptr && "Failed to load or generate image");
	}
	(void)channels;

	uint32_t instance_size = static_cast<uint32_t>(width * height * 4);
	uint32_t total_size = instance_size * layer_count;
	VE_LOGD("Texture paths read successfully");

	// Concatenate all texture data into a single contiguous buffer in CPU memory
	// TODO: skip this ?
	/*
	std::vector<uint8_t> combined_data(total_size);
	uint8_t* dest_ptr = combined_data.data();
	for (size_t i = 0; i < layer_count; i++) {
		std::memcpy(dest_ptr, pixels[i], instance_size);
		dest_ptr += instance_size;
	}
	VE_LOGD("Combined data size: " << combined_data.size() << ", expected: " << total_size);
	*/

	// Create a local scope staging buffer
	ve::VeBuffer staging_buffer(
		m_ve_device,
		instance_size,
		layer_count,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);

	// Write all data to staging buffer in one operation
	VE_LOGD("Buffer size: " << staging_buffer.getBufferSize() << ", expected: " << total_size);
	staging_buffer.map();
	for (size_t i = 0; i < layer_count; i++) {
		staging_buffer.writeToBuffer(pixels[i], instance_size, i * instance_size);
	}

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
		layer_count
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
		static_cast<uint32_t>(height),
		layer_count
	);

	// Transition image to be optimal for shader read access
	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::PipelineStageFlagBits2::eFragmentShader);

	// Free pixel data
	for (size_t i = 0; i < layer_count; i++) {
		if (texture_paths[i].filename() == "default_albedo.png" ||
		    texture_paths[i].filename() == "default_normal.png" ||
		    texture_paths[i].filename() == "default_metallic_roughness.png") {
			free(pixels[i]);
		} else {
			stbi_image_free(pixels[i]);
		}
	}
}

// Sets max anisotropy to the maximum value supported by the device or 16, whichever is lower
void VeTexture::createTextureSampler() {
	auto max_anisotropy = std::min(16.0f, m_ve_device.getDeviceProperties().limits.maxSamplerAnisotropy);
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eLinear,
		.addressModeU = vk::SamplerAddressMode::eRepeat,
		.addressModeV = vk::SamplerAddressMode::eRepeat,
		.addressModeW = vk::SamplerAddressMode::eRepeat,
		.mipLodBias = 0.0f,
		.anisotropyEnable = vk::True,
		.maxAnisotropy = max_anisotropy,
		.compareEnable = vk::False,
		.compareOp = vk::CompareOp::eAlways,
		.minLod = 0.0f,
		.maxLod = 0.0f,
		.borderColor = vk::BorderColor::eIntOpaqueBlack,
		.unnormalizedCoordinates = vk::False
	};
	m_texture_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
}

vk::raii::Sampler VeTexture::createDepthCompareSampler(VeDevice& device) {
	// MoltenVK doesn't support comparison samplers (mutableComparisonSamplers = false)
	// Disable compare mode on macOS - we'll do comparison in shader instead
	bool enable_compare = false;

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
		.compareEnable = enable_compare,
		.compareOp = vk::CompareOp::eLess,
		.minLod = 0.0f,
		.maxLod = 0.0f,
		.borderColor = vk::BorderColor::eFloatOpaqueWhite,  // outside shadow map = lit (clamp to border)
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
        }
    }
    return pixels;
}


vk::DescriptorImageInfo VeTexture::getDescriptorInfo() const {
	vk::DescriptorImageInfo image_info{
		.sampler = m_texture_sampler,
		.imageView = m_texture_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};
	return image_info;
}

}