#include "pch.hpp"
#include "ve_image.hpp"

namespace ve {
	VeImage::VeImage(
		VeDevice& ve_device,
		uint32_t width,
		uint32_t height,
		vk::Format format,
		vk::ImageTiling tiling,
		vk::ImageUsageFlags usage,
		vk::MemoryPropertyFlags properties,
		vk::ImageAspectFlags aspect_flags,
		bool is_cubemap)
		: m_ve_device(ve_device), m_width(width), m_height(height), m_format(format),
		  m_tiling(tiling), m_usage(usage), m_properties(properties),
		  m_aspect_flags(aspect_flags),
		  m_array_layers(is_cubemap ? 6 : 1) {

		if (is_cubemap) {
			m_image_create_flags |= vk::ImageCreateFlagBits::eCubeCompatible;
			m_image_view_type = vk::ImageViewType::eCube;
		}
		createImage();
		createImageView();

	}

	VeImage::~VeImage() {}

	// Hardcoded:
	// imageType, extent depth, mip, arraylayers, initlayout, sharingmode, samples, flags
	void VeImage::createImage() {
		assert(m_width > 0 && m_height > 0 && "Image width and height must be greater than zero");
		assert(m_usage != static_cast<vk::ImageUsageFlags>(0) && "Image usage flags must not be empty");
		// Create image
		vk::ImageCreateInfo image_info {
			.sType = vk::StructureType::eImageCreateInfo,
			.pNext = nullptr,
			.flags = m_image_create_flags,
			.imageType = vk::ImageType::e2D,
			.format = m_format,
			.extent = vk::Extent3D{ m_width, m_height, 1 },
			.mipLevels = 1,
			.arrayLayers = m_array_layers,
			.samples = vk::SampleCountFlagBits::e1,
			.tiling = m_tiling,
			.usage = m_usage,
			.sharingMode = vk::SharingMode::eExclusive,
			.queueFamilyIndexCount = 0,
			.pQueueFamilyIndices = nullptr,
			.initialLayout = vk::ImageLayout::eUndefined
		};
		m_image = vk::raii::Image(m_ve_device.getDevice(), image_info);
		assert(*m_image != VK_NULL_HANDLE && "Failed to create image");

		// Allocate and bind memory to image
		vk::MemoryRequirements mem_requirements = m_image.getMemoryRequirements();
		vk::MemoryAllocateInfo alloc_info {
			.sType = vk::StructureType::eMemoryAllocateInfo,
			.pNext = nullptr,
			.allocationSize = mem_requirements.size,
			.memoryTypeIndex = m_ve_device.findMemoryType(mem_requirements.memoryTypeBits, m_properties)
		};
		m_image_memory = vk::raii::DeviceMemory(m_ve_device.getDevice(), alloc_info);
		assert(*m_image_memory != VK_NULL_HANDLE && "Failed to allocate image memory");
		m_image.bindMemory(*m_image_memory, 0); // offset 0
	}

	void VeImage::createImageView() {
		assert(*m_image != VK_NULL_HANDLE && "Image must be valid when creating image view");
		vk::ImageViewCreateInfo view_info {
			.sType = vk::StructureType::eImageViewCreateInfo,
			.pNext = nullptr,
			.flags = {},
			.image = *m_image,
			.viewType = m_image_view_type,
			.format = m_format,
			.components = {},
			.subresourceRange = vk::ImageSubresourceRange {
				.aspectMask = m_aspect_flags,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = m_array_layers
			}
		};
		m_image_view = vk::raii::ImageView(m_ve_device.getDevice(), view_info);
		assert(*m_image_view != VK_NULL_HANDLE && "Failed to create image view");
	}

	// Hardcoded: src and dst queue family indices to ignored,
	// subresource range
	void VeImage::transitionImageLayout(
		vk::ImageLayout old_layout,
		vk::ImageLayout new_layout,
		vk::AccessFlags2 src_access_mask,
		vk::AccessFlags2 dst_access_mask,
		vk::PipelineStageFlags2 src_stage,
		vk::PipelineStageFlags2 dst_stage) {

		assert(*m_image != VK_NULL_HANDLE && "Image must be valid when transitioning image layout");
		QueueKind kind = QueueKind::Graphics;
		if (m_usage & vk::ImageUsageFlagBits::eTransferSrc || m_usage & vk::ImageUsageFlagBits::eTransferDst) {
			kind = QueueKind::Transfer;
		}
		auto command_buffer = m_ve_device.beginSingleTimeCommands(kind);
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
			.image = *m_image,
			.subresourceRange = {
				.aspectMask = m_aspect_flags,
				.baseMipLevel = 0,
				.levelCount = 1,
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
		command_buffer->pipelineBarrier2(dependency_info);
		m_ve_device.endSingleTimeCommands(*command_buffer, kind);
	}
} // namespace ve
