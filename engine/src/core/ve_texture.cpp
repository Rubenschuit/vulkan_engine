#include "pch.hpp"
#include "ve_texture.hpp"
#include "ve_buffer.hpp"

#include <ktx.h>

namespace ve {

VeTexture::VeTexture(VeDevice& ve_device, const std::filesystem::path& texture_path) : m_ve_device(ve_device) {
	createTextureImage(texture_path);
	createTextureSampler();
}

VeTexture::~VeTexture(){}

// TODO: mip levels
// Loads a texture from a file and creates a Vulkan image resources.
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
		is_cubemap

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

vk::DescriptorImageInfo VeTexture::getDescriptorInfo() const {
	vk::DescriptorImageInfo image_info{
		.sampler = m_texture_sampler,
		.imageView = m_texture_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};
	return image_info;
}

}