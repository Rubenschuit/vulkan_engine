#include "pch.hpp"
#include "ve_texture.hpp"
#include "ve_buffer.hpp"

#define STB_IMAGE_IMPLEMENTATION // include implementations, without: only prototypes
#include <stb_image.h>
#include <iostream>

namespace ve {
	VeTexture::VeTexture(VeDevice& ve_device, const char* texture_path) : ve_device(ve_device) {

		createTextureImage(texture_path);
		createTextureSampler();
	}

	VeTexture::~VeTexture(){}

	void VeTexture::createTextureImage(const char* texture_path) {
		// Load image from file using stb_image
    	stbi_uc* pixels = stbi_load(texture_path, &width, &height, &channels, STBI_rgb_alpha);
		if (!pixels)
			throw std::runtime_error("Failed to load " + std::string(texture_path));

		VE_LOGD("Texture channels: " << channels);
		// Create a local scope staging buffer
		ve::VeBuffer staging_buffer(
			ve_device,
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
		texture_image = std::make_unique<ve::VeImage>(
			ve_device,
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height),
			vk::Format::eR8G8B8A8Srgb,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor
		);
		// Next we execute synchronously 3 single-time command buffers:
		// TODO: consider combining these into one command buffer

		//transition image to be optimal for receiving data from buffer
		texture_image->transitionImageLayout(
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eTransferDstOptimal,
			{},
			vk::AccessFlagBits2::eTransferWrite,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eTransfer);

		// Copy data from staging buffer to texture image
		ve_device.copyBufferToImage(
			staging_buffer.getBuffer(),
			texture_image->getImage(),
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height));

		// Transition image to be optimal for shader read access
		texture_image->transitionImageLayout(
			vk::ImageLayout::eTransferDstOptimal,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eTransferWrite,
			vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTransfer,
			vk::PipelineStageFlagBits2::eFragmentShader);
	}

	// Sets max anisotropy to the maximum value supported by the device or 16, whichever is lower
	void VeTexture::createTextureSampler() {
		auto max_anisotropy = std::min(16.0f, ve_device.getDeviceProperties().limits.maxSamplerAnisotropy);
		vk::SamplerCreateInfo sampler_info{
			.magFilter = vk::Filter::eLinear,
			.minFilter = vk::Filter::eLinear,
			.addressModeU = vk::SamplerAddressMode::eRepeat,
			.addressModeV = vk::SamplerAddressMode::eRepeat,
			.addressModeW = vk::SamplerAddressMode::eRepeat,
			.anisotropyEnable = vk::True,
			.maxAnisotropy = max_anisotropy,
			.borderColor = vk::BorderColor::eIntOpaqueBlack,
			.unnormalizedCoordinates = vk::False,
			.compareEnable = vk::False,
			.compareOp = vk::CompareOp::eAlways,
			.mipmapMode = vk::SamplerMipmapMode::eLinear,
			.mipLodBias = 0.0f,
			.minLod = 0.0f,
			.maxLod = 0.0f
		};
		texture_sampler = vk::raii::Sampler(ve_device.getDevice(), sampler_info);
	}

}