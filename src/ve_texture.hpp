/* The VeTexture class is responsible for loading texture images
   and creating Vulkan image resources. */
#pragma once

#include "ve_device.hpp"
#include "ve_image.hpp"

namespace ve {
	class VeTexture {
	public:
		VeTexture(ve::VeDevice& device, const char* texture_path);
		~VeTexture();

		VeTexture(const VeTexture&) = delete;
		VeTexture& operator=(const VeTexture&) = delete;

		vk::raii::Sampler& getSampler() { return texture_sampler; };
		vk::raii::ImageView& getImageView() { return texture_image->getImageView(); };

	private:
		void createTextureImage(const char* texture_path);
		void createTextureSampler();

		ve::VeDevice& ve_device;
		int width;
		int height;
		int channels;
		std::unique_ptr<ve::VeImage> texture_image;

		// Todo:: move sampler outside of texture class if we want to sample multiple images
		vk::raii::Sampler texture_sampler{nullptr};
	};
} // namespace ve