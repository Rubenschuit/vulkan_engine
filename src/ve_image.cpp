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
		vk::ImageAspectFlags aspect_flags)
		: ve_device(ve_device), width(width), height(height), format(format),
		  tiling(tiling), usage(usage), properties(properties), aspect_flags(aspect_flags) {

		createImage();
		createImageView();

	}

	VeImage::~VeImage() {}

	// Hardcoded:
	// imageType, extent depth, mip, arraylayers, initlayout, sharingmode, samples, flags
	void VeImage::createImage() {
		assert(width > 0 && height > 0 && "Image width and height must be greater than zero");
		assert(usage != static_cast<vk::ImageUsageFlags>(0) && "Image usage flags must not be empty");
		// Create image
		vk::ImageCreateInfo image_info {
			.sType = vk::StructureType::eImageCreateInfo,
			.imageType = vk::ImageType::e2D,
			.extent = vk::Extent3D{ width, height, 1 },
			.mipLevels = 1,
			.arrayLayers = 1,
			.format = format,
			.tiling = tiling,
			.initialLayout = vk::ImageLayout::eUndefined,
			.usage = usage,
			.sharingMode = vk::SharingMode::eExclusive,
			.samples = vk::SampleCountFlagBits::e1,
			.flags = {}
		};
		image = vk::raii::Image(ve_device.getDevice(), image_info);
		assert(*image != VK_NULL_HANDLE && "Failed to create image");

		// Allocate and bind memory to image
		vk::MemoryRequirements mem_requirements = image.getMemoryRequirements();
		vk::MemoryAllocateInfo alloc_info {
			.sType = vk::StructureType::eMemoryAllocateInfo,
			.allocationSize = mem_requirements.size,
			.memoryTypeIndex = ve_device.findMemoryType(mem_requirements.memoryTypeBits, properties)
		};
		image_memory = vk::raii::DeviceMemory(ve_device.getDevice(), alloc_info);
		assert(*image_memory != VK_NULL_HANDLE && "Failed to allocate image memory");
		image.bindMemory(*image_memory, 0); // offset 0
	}

	void VeImage::createImageView() {
		assert(*image != VK_NULL_HANDLE && "Image must be valid when creating image view");
		vk::ImageViewCreateInfo view_info {
			.sType = vk::StructureType::eImageViewCreateInfo,
			.image = *image,
			.viewType = vk::ImageViewType::e2D,
			.format = format,
			.subresourceRange = vk::ImageSubresourceRange {
				.aspectMask = aspect_flags,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};
		image_view = vk::raii::ImageView(ve_device.getDevice(), view_info);
		assert(*image_view != VK_NULL_HANDLE && "Failed to create image view");
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

		assert(*image != VK_NULL_HANDLE && "Image must be valid when transitioning image layout");
		QueueKind kind = QueueKind::Graphics;
		if (usage & vk::ImageUsageFlagBits::eTransferSrc || usage & vk::ImageUsageFlagBits::eTransferDst) {
			kind = QueueKind::Transfer;
		}
		auto command_buffer = ve_device.beginSingleTimeCommands(kind);
		vk::ImageMemoryBarrier2 barrier = {
			.srcStageMask = src_stage,
			.srcAccessMask = src_access_mask,
			.dstStageMask = dst_stage,
			.dstAccessMask = dst_access_mask,
			.oldLayout = old_layout,
			.newLayout = new_layout,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = *image,
			.subresourceRange = {
				.aspectMask = aspect_flags,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};
		vk::DependencyInfo dependency_info = {
			.dependencyFlags = {},
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &barrier
		};
		command_buffer->pipelineBarrier2(dependency_info);
		ve_device.endSingleTimeCommands(*command_buffer, kind);
	}
} // namespace ve
