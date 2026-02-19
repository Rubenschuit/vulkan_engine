/* VeImage is an encapsulation of a Vulkan image and its associated resources. */
#pragma once
#include "ve_export.hpp"
#include "vulkan/ve_device.hpp"
#include "utils/ve_log.hpp"

// Hardcoded:
// imageType, extent depth, mip, arraylayers, initlayout, sharingmode, samples, flags
namespace ve {

class VENGINE_API VeImage {
public:
	VeImage(
		VeDevice& ve_device,
		uint32_t width,
		uint32_t height,
		vk::SampleCountFlagBits num_samples,
		vk::Format format,
		vk::ImageTiling tiling,
		vk::ImageUsageFlags usage,
		vk::MemoryPropertyFlags properties,
		vk::ImageAspectFlags aspect_flags,
		bool is_cubemap,
		uint32_t array_layers,
		uint32_t mip_levels = 1
	);
	~VeImage();

	VeImage(const VeImage&) = delete;
	VeImage& operator=(const VeImage&) = delete;

	const vk::raii::Image& getImage() const { return m_image; }
	const vk::raii::ImageView& getImageView() const { return m_image_view; }
	const vk::raii::DeviceMemory& getImageMemory() const { return m_image_memory; }
	vk::Format getFormat() const { return m_format; }
	uint32_t getWidth() const { return m_width; }
	uint32_t getHeight() const { return m_height; }
	uint32_t getMipLevels() const { return m_mip_levels; }
	vk::ImageAspectFlags getAspectFlags() const { return m_aspect_flags; }
	vk::Extent2D getExtent2D() const { return vk::Extent2D{ m_width, m_height }; }

	// Create an image view for a specific layer (for rendering to array layers)
	vk::raii::ImageView createLayerImageView(uint32_t layer) const;

	// Create a 2DArray image view spanning multiple contiguous layers (for multiview rendering)
	vk::raii::ImageView createMultiLayerImageView(uint32_t base_layer, uint32_t layer_count) const;

	//debug
	void printDebugInfo() const {
		if (m_image_view_type == vk::ImageViewType::e2D)
			VE_LOGD("Image view type: 2D");
		else if (m_image_view_type == vk::ImageViewType::e2DArray)
			VE_LOGD("Image view type: 2DArray");
		else if (m_image_view_type == vk::ImageViewType::eCube)
			VE_LOGD("Image view type: Cube");
		else
			VE_LOGD("Image view type: Unknown");
	}


	void transitionImageLayout(
		vk::ImageLayout old_layout,
		vk::ImageLayout new_layout,
		vk::AccessFlags2 src_access_mask,
		vk::AccessFlags2 dst_access_mask,
		vk::PipelineStageFlags2 src_stage,
		vk::PipelineStageFlags2 dst_stage
	);

	void transitionImageLayout(
		vk::raii::CommandBuffer& command_buffer,
		vk::ImageLayout old_layout,
		vk::ImageLayout new_layout,
		vk::AccessFlags2 src_access_mask,
		vk::AccessFlags2 dst_access_mask,
		vk::PipelineStageFlags2 src_stage,
		vk::PipelineStageFlags2 dst_stage
	);

private:
	void createImage();
	void createImageView();

	VeDevice& m_ve_device;
	uint32_t m_width;
	uint32_t m_height;
	vk::SampleCountFlagBits m_num_samples;
	vk::Format m_format;
	vk::ImageTiling m_tiling;
	vk::ImageUsageFlags m_usage;
	vk::MemoryPropertyFlags m_properties;
	vk::ImageAspectFlags m_aspect_flags;
	uint32_t m_array_layers;
	uint32_t m_mip_levels;
	vk::ImageCreateFlags m_image_create_flags{};
	vk::ImageViewType m_image_view_type{vk::ImageViewType::e2D};

	vk::raii::Image m_image{nullptr};
	vk::raii::DeviceMemory m_image_memory{nullptr};
	vk::raii::ImageView m_image_view{nullptr};


};
} // namespace ve
