#include "ve_swap_chain.hpp"

#include <stdexcept>
#include <array>
#include <iostream>
#include <set>
#include <limits>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cassert>

namespace ve {

	VeSwapChain::VeSwapChain(VeDevice& device, vk::Extent2D window_extent)
		: ve_device(device), window_extent(window_extent) {
		init();
	}

	VeSwapChain::VeSwapChain(VeDevice& device, vk::Extent2D window_extent, std::shared_ptr<VeSwapChain> old_swap_chain)
		: ve_device(device), window_extent(window_extent), old_swap_chain(old_swap_chain) {
		init();
		// destroy old swap chain AFTER the new one is ready
		old_swap_chain = nullptr;
	}

	VeSwapChain::~VeSwapChain() {
		swap_chain_image_views.clear();
		swap_chain = nullptr;
	}

	void VeSwapChain::init() {
		createSwapChain();
		createSwapChainImageViews();
		createDepthResources();
		createSyncObjects();
	}

	vk::Result VeSwapChain::acquireNextImage(uint32_t* image_index) {
		// Signal the semaphore once the image is available
		auto [result, _image_index] = swap_chain.acquireNextImage(
			UINT64_MAX,
			present_complete_semaphores[semaphore_index],
			nullptr);
		*image_index = _image_index;
		return result;
	}

	vk::Result VeSwapChain::submitAndPresent(vk::CommandBuffer commandBuffer, uint32_t* image_index) {
		vk::PipelineStageFlags wait_dest_stage_mask(vk::PipelineStageFlagBits::eColorAttachmentOutput);
		const vk::SubmitInfo submit_info{
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &*present_complete_semaphores[semaphore_index],
			.pWaitDstStageMask = &wait_dest_stage_mask,
			.commandBufferCount = 1,
			.pCommandBuffers = &commandBuffer,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &*render_finished_semaphores[*image_index]
		};

		// Submit the command buffer to the graphics queue and signal the fence when it is done
		ve_device.getQueue().submit(submit_info, *in_flight_fences[current_frame]);

		const vk::PresentInfoKHR present_info{
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &*render_finished_semaphores[*image_index],
			.swapchainCount = 1,
			.pSwapchains = &*swap_chain,
			.pImageIndices = image_index
		};

		// Present the image to the screen
		return ve_device.getQueue().presentKHR(present_info);
	}

	void VeSwapChain::createSwapChain() {
		swap_chain_support = ve_device.getSwapChainSupport();
		surface_format = chooseSwapSurfaceFormat(swap_chain_support.formats);
		present_mode = chooseSwapPresentMode(swap_chain_support.presentModes);
		swap_chain_extent = chooseSwapExtent(swap_chain_support.capabilities);
		swap_chain_image_format = surface_format.format;

		// try to use one more than the minimum number of images to improve gpu utilization
		// note that there is no guarantee that we can get that many images, so we need to check against the maximum as well
		uint32_t image_count = swap_chain_support.capabilities.minImageCount + 1;
		if (swap_chain_support.capabilities.maxImageCount > 0 && image_count > swap_chain_support.capabilities.maxImageCount) {
			image_count = swap_chain_support.capabilities.maxImageCount;
		}

		vk::SwapchainCreateInfoKHR create_info{
			.sType = vk::StructureType::eSwapchainCreateInfoKHR,
			.surface = *ve_device.getSurface(),
			.minImageCount = image_count,
			.imageFormat = surface_format.format,
			.imageColorSpace = surface_format.colorSpace,
			.imageExtent = swap_chain_extent,
			.imageArrayLayers = 1, // always 1 unless developing stereoscopic 3D app
			.imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
			.preTransform = swap_chain_support.capabilities.currentTransform,
			.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
			.presentMode = present_mode,
			.clipped = VK_TRUE,
			.oldSwapchain = VK_NULL_HANDLE, // may be overwritten below if recreating
			// graphics and presentation are in the same queue family, so we can use exclusive mode
			.imageSharingMode = vk::SharingMode::eExclusive,
			.queueFamilyIndexCount = 0, // optional
			.pQueueFamilyIndices = nullptr // optional
		};

		// If we are recreating (old_swap_chain retained), provide its handle so the driver
		// can safely migrate resources and release the old one.
		if (old_swap_chain != nullptr) {
			// Dereference twice: first to get vk::raii::SwapchainKHR&, second to obtain raw handle
			create_info.oldSwapchain = **old_swap_chain->getSwapChain();
		}

		swap_chain = vk::raii::SwapchainKHR(ve_device.getDevice(), create_info);
		swap_chain_images = swap_chain.getImages();
	}

	void VeSwapChain::createSwapChainImageViews() {
		assert(swap_chain_image_views.empty());
		vk::ImageViewCreateInfo imageViewCreateInfo{
			.viewType = vk::ImageViewType::e2D,
			.format = surface_format.format,
			.subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }
		};
		for (auto image : swap_chain_images) {
			imageViewCreateInfo.image = image;
			// call the constructor of vk::raii::ImageView and add it to the vector
			swap_chain_image_views.emplace_back(ve_device.getDevice(), imageViewCreateInfo);
		}
	}

	void VeSwapChain::createDepthResources() {
		vk::Format depth_format = ve_device.findDepthFormat();
		depth_image = std::make_unique<VeImage>(
			ve_device,
			swap_chain_extent.width,
			swap_chain_extent.height,
			depth_format,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eDepthStencilAttachment,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eDepth);

		// transition the depth image to be optimal for a depth attachment using a single-time command buffer
		{
			auto cmd = ve_device.beginSingleTimeCommands();
			depth_image->transitionImageLayout(
				cmd,
				vk::ImageLayout::eUndefined,
				vk::ImageLayout::eDepthStencilAttachmentOptimal,
				{},
				vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
				vk::PipelineStageFlagBits2::eTopOfPipe,
				vk::PipelineStageFlagBits2::eEarlyFragmentTests);
			ve_device.endSingleTimeCommands(cmd);
		}
	}

	// Create 2 semaphores and 1 fence per frame in flight
	void VeSwapChain::createSyncObjects() {
		present_complete_semaphores.clear();
		render_finished_semaphores.clear();
		in_flight_fences.clear();

		vk::SemaphoreCreateInfo present_info{};
		vk::SemaphoreCreateInfo render_info{};
		vk::FenceCreateInfo fence_info{ .flags = vk::FenceCreateFlagBits::eSignaled };

		for (size_t i = 0; i < swap_chain_images.size(); i++) {
			present_complete_semaphores.emplace_back(ve_device.getDevice(), present_info);
			render_finished_semaphores.emplace_back(ve_device.getDevice(), render_info);
		}

		for (size_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; i++) {
			in_flight_fences.emplace_back(ve_device.getDevice(), fence_info);
		}
	}

	// Choose the surface format, which is a combination of color depth and color space.
	// Prefer 8-bit BGRA and SRGB nonlinear color space for now.
	vk::SurfaceFormatKHR VeSwapChain::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available_formats) {
		for (const auto& available_format : available_formats) {
			if (available_format.format == vk::Format::eB8G8R8A8Srgb &&
				available_format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
				return available_format;
			}
		}
		return available_formats[0];
	}

	// Choose the presentation mode, which is how images are presented to the screen.
	// Mailbox is the lowest latency non-tearing mode, but may not be available.
	// Fifo is always available, but may have higher latency.
	// Immediate may be available, and is lowest latency, but may have tearing.
	vk::PresentModeKHR VeSwapChain::chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& available_present_modes) {
		for (const auto& available_present_mode : available_present_modes) {
			if (available_present_mode == vk::PresentModeKHR::eImmediate) {
				return available_present_mode;
			}
		}
		return vk::PresentModeKHR::eFifo;
	}

	// Choose the resolution (pixels) of the swap chain images.
	// If the surface size is defined, the swap chain size must match.
	// If the surface size is undefined, the swap chain size can be set to
	// the size of the window, but must be within the min and max bounds.
	vk::Extent2D VeSwapChain::chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities) {
		if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
			return capabilities.currentExtent;
		} else {
			vk::Extent2D actual_extent = window_extent;
			actual_extent.width = std::clamp(actual_extent.width,
											 capabilities.minImageExtent.width,
											 capabilities.maxImageExtent.width);
			actual_extent.height = std::clamp(actual_extent.height,
											  capabilities.minImageExtent.height,
											  capabilities.maxImageExtent.height);
			return actual_extent;
		}
	}

	void VeSwapChain::waitForFences() {
		while (vk::Result::eTimeout ==
			   ve_device.getDevice().waitForFences(*in_flight_fences[current_frame], vk::True, UINT64_MAX));
		return;
	}

	// Reset the fence of the current frame back to unsignaled state
	void VeSwapChain::resetFences() {
		ve_device.getDevice().resetFences(*in_flight_fences[current_frame]);
	}

	void VeSwapChain::advanceFrame() {
		current_frame = (current_frame + 1) % ve::MAX_FRAMES_IN_FLIGHT;
		semaphore_index = (semaphore_index + 1) % swap_chain_images.size();
	}

		// Transition the image layout of the given swap chain image using
	// pipeline barriers to ensure proper synchronization
	void VeSwapChain::transitionImageLayout(
				vk::raii::CommandBuffer& command_buffer,
				uint32_t image_index,
				vk::ImageLayout old_layout,
				vk::ImageLayout new_layout,
				vk::AccessFlags2 src_access_mask,
				vk::AccessFlags2 dst_access_mask,
				vk::PipelineStageFlags2 src_stage_mask,
				vk::PipelineStageFlags2 dst_stage_mask) {

		assert(image_index < getImageCount() && "Image index out of bounds");
		vk::ImageMemoryBarrier2 barrier = {
			.srcStageMask = src_stage_mask,
			.srcAccessMask = src_access_mask,
			.dstStageMask = dst_stage_mask,
			.dstAccessMask = dst_access_mask,
			.oldLayout = old_layout,
			.newLayout = new_layout,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = swap_chain_images[image_index],
			.subresourceRange = {
				.aspectMask = vk::ImageAspectFlagBits::eColor,
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
		command_buffer.pipelineBarrier2(dependency_info);
	}
}

