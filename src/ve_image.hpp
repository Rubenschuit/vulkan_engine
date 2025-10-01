#pragma once

#include "ve_device.hpp"

// Hardcoded:
// imageType, extent depth, mip, arraylayers, initlayout, sharingmode, samples, flags
namespace ve {
	class VeImage {
	public:
		VeImage(
			VeDevice& ve_device,
			uint32_t width,
			uint32_t height,
			vk::Format format,
			vk::ImageTiling tiling,
			vk::ImageUsageFlags usage,
			vk::MemoryPropertyFlags properties,
			vk::ImageAspectFlags aspect_flags);
		~VeImage();

		VeImage(const VeImage&) = delete;
		VeImage& operator=(const VeImage&) = delete;

		vk::raii::Image& getImage() { return image; }
		vk::raii::ImageView& getImageView() { return image_view; }
		vk::raii::DeviceMemory& getImageMemory() { return image_memory; }
		vk::Format getFormat() const { return format; }
		uint32_t getWidth() const { return width; }
		uint32_t getHeight() const { return height; }
		vk::ImageAspectFlags getAspectFlags() const { return aspect_flags; }
		vk::Extent2D getExtent2D() const { return vk::Extent2D{ width, height }; }

		void transitionImageLayout(
			vk::ImageLayout old_layout,
			vk::ImageLayout new_layout,
			vk::AccessFlags2 src_access_mask,
			vk::AccessFlags2 dst_access_mask,
			vk::PipelineStageFlags2 src_stage,
			vk::PipelineStageFlags2 dst_stage);

	private:
		void createImage();
		void createImageView();

		VeDevice& ve_device;
		uint32_t width;
		uint32_t height;
		vk::Format format;
		vk::ImageTiling tiling;
		vk::ImageUsageFlags usage;
		vk::MemoryPropertyFlags properties;
		vk::ImageAspectFlags aspect_flags;

		vk::raii::Image image{nullptr};
		vk::raii::DeviceMemory image_memory{nullptr};
		vk::raii::ImageView image_view{nullptr};


	};
} // namespace ve
