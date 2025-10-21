#include "pch.hpp"
#include "ve_texture.hpp"
#include "ve_buffer.hpp"

#define STB_IMAGE_IMPLEMENTATION // include implementations, without: only prototypes
#include <stb_image.h>
#include <iostream>

namespace ve {

VeTexture::VeTexture(VeDevice& ve_device, std::string texture_path) : m_ve_device(ve_device) {
	createTextureImage(texture_path);
	createTextureSampler();
}
VeTexture::VeTexture(VeDevice& ve_device, std::vector<std::string> texture_paths) : m_ve_device(ve_device) {

	createCubeTextureImage(texture_paths);
	createTextureSampler();
}

VeTexture::~VeTexture(){}

void VeTexture::createTextureImage(std::string texture_path) {
	// Load image from file using stb_image; on failure, create a 1x1 white fallback
	stbi_uc* pixels = stbi_load(texture_path.data(), &m_width, &m_height, &m_channels, STBI_rgb_alpha);
	bool free_pixels = true;
	std::vector<stbi_uc> fallback_pixels;
	if (!pixels) {
		VE_LOGW("Texture not found, using fallback: " << texture_path);
		m_width = 1;
		m_height = 1;
		m_channels = 4;
		fallback_pixels.assign({255, 255, 255, 255}); // 1x1 white RGBA
		pixels = fallback_pixels.data();
		free_pixels = false; // do not free fallback memory with stbi
	}

	// texture loaded
	// Create a local scope staging buffer
	ve::VeBuffer staging_buffer(
		m_ve_device,
		4,                                        // instance size
		static_cast<uint32_t>(m_width * m_height),    // instance count
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);

	// Copy image data to staging buffer
	staging_buffer.map();
	staging_buffer.writeToBuffer((void*)pixels);
	// unmap is called in the destructor of VeBuffer
	if (free_pixels) {
		stbi_image_free(pixels);
	}

	// Create image
	m_texture_image = std::make_unique<ve::VeImage>(
		m_ve_device,
		static_cast<uint32_t>(m_width),
		static_cast<uint32_t>(m_height),
		vk::Format::eR8G8B8A8Srgb,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor
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
		static_cast<uint32_t>(m_width),
		static_cast<uint32_t>(m_height)
	);

	// Transition image to be optimal for shader read access
	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::PipelineStageFlagBits2::eFragmentShader);
}

void VeTexture::createCubeTextureImage(std::vector<std::string> texture_paths) {
	assert(texture_paths.size() == 6 && "Cubemap requires 6 texture paths");
	stbi_uc* pixels[6];
	int face_w = 0, face_h = 0, face_c = 0;
	for (size_t i = 0; i < 6; i++) {
		VE_LOGI("Loading cube map face: " << texture_paths[i]);
		int w = 0, h = 0, c = 0;
		// Force RGBA (4 bytes per pixel) for all faces
		pixels[i] = stbi_load(texture_paths[i].data(), &w, &h, &c, STBI_rgb_alpha);
		assert(pixels[i] != nullptr && "Failed to load cube map face texture");
		if (i == 0) {
			face_w = w; face_h = h; face_c = 4; // after conversion
		} else {
			// All faces of a cubemap must match dimensions
			assert(w == face_w && h == face_h && "Cubemap faces must have identical dimensions");
		}
	}
	// Store dimensions in members
	m_width = face_w;
	m_height = face_h;
	m_channels = face_c;

	// textures loaded
	// Create a local scope staging buffer
	ve::VeBuffer staging_buffer(
		m_ve_device,
		4,                                        // instance size
		static_cast<uint32_t>(m_width * m_height * 6),    // instance count
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	VE_LOGD("Created staging buffer for cube map with size: " << (m_width * m_height * 6 * 4) << " bytes");

	// Copy image data to staging buffer (concatenate the 6 faces)
	staging_buffer.map();
	const vk::DeviceSize layer_size = static_cast<vk::DeviceSize>(m_width) * static_cast<vk::DeviceSize>(m_height) * 4;
	for (int i = 0; i < 6; ++i) {
		assert(pixels[i] != nullptr && "Cubemap face pixels are null");
		const vk::DeviceSize offset = layer_size * static_cast<vk::DeviceSize>(i);
		staging_buffer.writeToBuffer((void*)pixels[i], layer_size, offset);
	}
	// unmap is called in the destructor of VeBuffer

	VE_LOGD("Copied cube map data to staging buffer");

	for (int i = 0; i < 6; i++) {
		stbi_image_free(pixels[i]);
	}

	// Create image
	m_texture_image = std::make_unique<ve::VeImage>(
		m_ve_device,
		static_cast<uint32_t>(m_width),
		static_cast<uint32_t>(m_height),
		vk::Format::eR8G8B8A8Srgb,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		true // is cubemap
	);
	// Next we execute synchronously 3 single-time command buffers:
	// TODO: consider combining these into one command buffer

	VE_LOGD("Created cube map image");

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
		static_cast<uint32_t>(m_width),
		static_cast<uint32_t>(m_height),
		6
	);

	// Transition image to be optimal for shader read access
	m_texture_image->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::PipelineStageFlagBits2::eFragmentShader);
	VE_LOGD("Transitioned cube map image layout");
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