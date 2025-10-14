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

		const vk::raii::Sampler& getSampler() const { return m_texture_sampler; };
		const vk::raii::ImageView& getImageView() const { return m_texture_image->getImageView(); };
		vk::DescriptorImageInfo getDescriptorInfo() const;

	private:
		void createTextureImage(const char* texture_path);
		void createTextureSampler();

		ve::VeDevice& m_ve_device;
		int m_width;
		int m_height;
		int m_channels;
		std::unique_ptr<ve::VeImage> m_texture_image;

		// Todo:: move sampler outside of texture class if we want to sample multiple images
		vk::raii::Sampler m_texture_sampler{nullptr};
	};
} // namespace ve