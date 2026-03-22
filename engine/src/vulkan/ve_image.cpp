#include "pch.hpp"
#include "vulkan/ve_image.hpp"
#include <vk_mem_alloc.h>

namespace ve {

VeImage::VeImage(
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
	uint32_t mip_levels)
	: m_ve_device(ve_device), m_width(width), m_height(height), m_num_samples(num_samples),
		m_format(format), m_tiling(tiling), m_usage(usage), m_properties(properties),
		m_aspect_flags(aspect_flags), m_array_layers(array_layers), m_mip_levels(mip_levels) {

	if (is_cubemap) {
		m_image_create_flags |= vk::ImageCreateFlagBits::eCubeCompatible;
		m_image_view_type = vk::ImageViewType::eCube;
	}
	else if (array_layers > 1) {
		m_image_view_type = vk::ImageViewType::e2DArray;
	}
	else {
		assert(array_layers == 1 && "Array layers must be positve non zero integer");
		m_image_view_type = vk::ImageViewType::e2D;
	}
	createImage();
	createImageView();
}

VeImage::~VeImage() {
	m_image_view = vk::raii::ImageView{nullptr};
	if (m_allocation)
		vmaDestroyImage(m_ve_device.getAllocator(), static_cast<VkImage>(m_image), m_allocation);
}

void VeImage::createImage() {
	assert(m_width > 0 && m_height > 0 && "Image width and height must be greater than zero");
	assert(m_usage != static_cast<vk::ImageUsageFlags>(0) && "Image usage flags must not be empty");

	auto unique_families = m_ve_device.getQueueFamilyIndices().uniqueFamilies();
	bool use_concurrent = !m_ve_device.getQueueFamilyIndices().allSameFamily() && unique_families.size() > 1;

	VkImageCreateInfo image_info {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.flags = static_cast<VkImageCreateFlags>(m_image_create_flags),
		.imageType = VK_IMAGE_TYPE_2D,
		.format = static_cast<VkFormat>(m_format),
		.extent = VkExtent3D{ m_width, m_height, 1 },
		.mipLevels = m_mip_levels,
		.arrayLayers = m_array_layers,
		.samples = static_cast<VkSampleCountFlagBits>(m_num_samples),
		.tiling = static_cast<VkImageTiling>(m_tiling),
		.usage = static_cast<VkImageUsageFlags>(m_usage),
		.sharingMode = use_concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = use_concurrent ? static_cast<uint32_t>(unique_families.size()) : 0u,
		.pQueueFamilyIndices = use_concurrent ? unique_families.data() : nullptr,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VmaAllocationCreateInfo alloc_info {
		.usage = VMA_MEMORY_USAGE_AUTO,
	};

	VkImage vk_image;
	if (vmaCreateImage(m_ve_device.getAllocator(), &image_info, &alloc_info,
	                   &vk_image, &m_allocation, nullptr) != VK_SUCCESS)
		throw std::runtime_error("VMA: failed to create image");

	m_image = vk::Image(vk_image);
	assert(m_image && "Failed to create image");
}

void VeImage::createImageView() {
	assert(m_image && "Image must be valid when creating image view");
	vk::ImageViewCreateInfo view_info {
		.sType = vk::StructureType::eImageViewCreateInfo,
		.pNext = nullptr,
		.flags = {},
		.image = m_image,
		.viewType = m_image_view_type,
		.format = m_format,
		.components = {},
		.subresourceRange = vk::ImageSubresourceRange {
			.aspectMask = m_aspect_flags,
			.baseMipLevel = 0,
			.levelCount = m_mip_levels,
			.baseArrayLayer = 0,
			.layerCount = m_array_layers
		}
	};
	m_image_view = vk::raii::ImageView(m_ve_device.getDevice(), view_info);
	assert(*m_image_view != VK_NULL_HANDLE && "Failed to create image view");
}

vk::raii::ImageView VeImage::createLayerImageView(uint32_t layer) const {
	assert(m_image && "Image must be valid when creating layer image view");
	assert(layer < m_array_layers && "Layer index out of bounds");
	vk::ImageViewCreateInfo view_info {
		.sType = vk::StructureType::eImageViewCreateInfo,
		.pNext = nullptr,
		.flags = {},
		.image = m_image,
		.viewType = vk::ImageViewType::e2D,
		.format = m_format,
		.components = {},
		.subresourceRange = vk::ImageSubresourceRange {
			.aspectMask = m_aspect_flags,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = layer,
			.layerCount = 1
		}
	};
	return vk::raii::ImageView(m_ve_device.getDevice(), view_info);
}

vk::raii::ImageView VeImage::createMultiLayerImageView(uint32_t base_layer, uint32_t layer_count) const {
	assert(m_image && "Image must be valid when creating multi-layer image view");
	assert(base_layer + layer_count <= m_array_layers && "Layer range out of bounds");
	assert(layer_count > 1 && "Use createLayerImageView for single layers");
	vk::ImageViewCreateInfo view_info {
		.sType = vk::StructureType::eImageViewCreateInfo,
		.pNext = nullptr,
		.flags = {},
		.image = m_image,
		.viewType = vk::ImageViewType::e2DArray,
		.format = m_format,
		.components = {},
		.subresourceRange = vk::ImageSubresourceRange {
			.aspectMask = m_aspect_flags,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = base_layer,
			.layerCount = layer_count
		}
	};
	return vk::raii::ImageView(m_ve_device.getDevice(), view_info);
}

void VeImage::transitionImageLayout(
	vk::ImageLayout old_layout,
	vk::ImageLayout new_layout,
	vk::AccessFlags2 src_access_mask,
	vk::AccessFlags2 dst_access_mask,
	vk::PipelineStageFlags2 src_stage,
	vk::PipelineStageFlags2 dst_stage) {

	assert(m_image && "Image must be valid when transitioning image layout");

	constexpr auto kTransferStages =
		vk::PipelineStageFlagBits2::eTransfer |
		vk::PipelineStageFlagBits2::eAllTransfer |
		vk::PipelineStageFlagBits2::eTopOfPipe |
		vk::PipelineStageFlagBits2::eBottomOfPipe |
		vk::PipelineStageFlagBits2::eHost |
		vk::PipelineStageFlagBits2::eAllCommands;

	bool has_transfer_usage = (m_usage & vk::ImageUsageFlagBits::eTransferSrc) || (m_usage & vk::ImageUsageFlagBits::eTransferDst);
	bool stages_ok = !(src_stage & ~kTransferStages) && !(dst_stage & ~kTransferStages);

	QueueKind kind = (has_transfer_usage && stages_ok) ? QueueKind::Transfer : QueueKind::Graphics;
	auto command_buffer = m_ve_device.beginSingleTimeCommands(kind);
	transitionImageLayout(*command_buffer, old_layout, new_layout, src_access_mask, dst_access_mask, src_stage, dst_stage);
	m_ve_device.endSingleTimeCommands(*command_buffer, kind);
}

void VeImage::transitionImageLayout(
	vk::raii::CommandBuffer& command_buffer,
	vk::ImageLayout old_layout,
	vk::ImageLayout new_layout,
	vk::AccessFlags2 src_access_mask,
	vk::AccessFlags2 dst_access_mask,
	vk::PipelineStageFlags2 src_stage,
	vk::PipelineStageFlags2 dst_stage) {

	assert(m_image && "Image must be valid when transitioning image layout");
	vk::ImageMemoryBarrier2 barrier = {
		.sType = vk::StructureType::eImageMemoryBarrier2,
		.pNext = nullptr,
		.srcStageMask = src_stage,
		.srcAccessMask = src_access_mask,
		.dstStageMask = dst_stage,
		.dstAccessMask = dst_access_mask,
		.oldLayout = old_layout,
		.newLayout = new_layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_image,
		.subresourceRange = {
			.aspectMask = m_aspect_flags,
			.baseMipLevel = 0,
			.levelCount = m_mip_levels,
			.baseArrayLayer = 0,
			.layerCount = m_array_layers
		}
	};
	vk::DependencyInfo dependency_info = {
		.sType = vk::StructureType::eDependencyInfo,
		.pNext = nullptr,
		.dependencyFlags = {},
		.memoryBarrierCount = 0,
		.pMemoryBarriers = nullptr,
		.bufferMemoryBarrierCount = 0,
		.pBufferMemoryBarriers = nullptr,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &barrier
	};
	command_buffer.pipelineBarrier2(dependency_info);
}
} // namespace ve
