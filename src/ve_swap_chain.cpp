#include "pch.hpp"
#include "ve_swap_chain.hpp"

#include <stdexcept>
#include <array>
#include <set>
#include <limits>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cassert>

namespace ve {

VeSwapChain::VeSwapChain(VeDevice& device, vk::Extent2D window_extent)
	: m_ve_device(device), m_window_extent(window_extent) {
	init();
}

VeSwapChain::VeSwapChain(VeDevice& device, vk::Extent2D window_extent, std::shared_ptr<VeSwapChain> old_swap_chain)
	: m_ve_device(device), m_window_extent(window_extent), m_old_swap_chain(old_swap_chain) {
	init();
	// destroy old swap chain AFTER the new one is ready
	m_old_swap_chain = nullptr;
}

VeSwapChain::~VeSwapChain() {
	m_swap_chain_image_views.clear();
	m_swap_chain = nullptr;
}

void VeSwapChain::init() {
	createSwapChain();
	createSwapChainImageViews();
	createDepthResources();
	createSyncObjects();
}

vk::Result VeSwapChain::acquireNextImage(uint32_t* image_index) {
	// Signals the image-available semaphore (GPU side)
	auto [result, _image_index] = m_swap_chain.acquireNextImage(
		UINT64_MAX,
		*m_image_available_semaphores[m_current_frame],
		nullptr
	);
	*image_index = _image_index;
	// image acquired
	return result;
}

void VeSwapChain::submitComputeWork(vk::CommandBuffer command_buffer) {
	const vk::TimelineSemaphoreSubmitInfo timeline_info{
		.sType = vk::StructureType::eTimelineSemaphoreSubmitInfo,
		.pNext = nullptr,
		.waitSemaphoreValueCount = 1,
		.pWaitSemaphoreValues = &compute_wait_value,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &compute_signal_value
	};
	vk::PipelineStageFlags wait_stages[] = {vk::PipelineStageFlagBits::eComputeShader};
	vk::SubmitInfo submit_info{
		.pNext = &timeline_info,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &*semaphore,
		.pWaitDstStageMask = wait_stages,
		.commandBufferCount = 1,
		.pCommandBuffers = &command_buffer,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &*semaphore
	};

	// Submit the command buffer to the compute queue and signal the fence when it is done
	m_ve_device.getComputeQueue().submit(submit_info, nullptr);
}

vk::Result VeSwapChain::submitAndPresent(vk::CommandBuffer command_buffer, uint32_t* image_index) {
	// Wait on image-available (binary) and compute timeline before starting graphics work.
	vk::PipelineStageFlags wait_stages[2] = {
		vk::PipelineStageFlagBits::eColorAttachmentOutput, // swapchain image usage
		vk::PipelineStageFlagBits::eVertexInput            // instanced vertex buffer reads
	};
	// We will signal two semaphores (timeline + binary). For timeline submit info,
	// signalSemaphoreValueCount must equal signalSemaphoreCount when any signaled semaphore is a timeline.
	// Provide a dummy 0 for the binary semaphore; it will be ignored.
	std::array<uint64_t, 2> signal_values{ graphics_signal_value, uint64_t{0} };
	// For waits, include 0 for the binary and the expected value for the timeline.
	std::array<uint64_t, 2> wait_values{ uint64_t{0}, graphics_wait_value };
	vk::TimelineSemaphoreSubmitInfo timeline_info{
		.sType = vk::StructureType::eTimelineSemaphoreSubmitInfo,
		.pNext = nullptr,
		.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size()),
		.pWaitSemaphoreValues = wait_values.data(),
		.signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size()),
		.pSignalSemaphoreValues = signal_values.data()
	};

	// Wait on image-available (binary) and the timeline semaphore
	std::array<vk::Semaphore, 2> wait_sems{ *m_image_available_semaphores[m_current_frame], *semaphore };
	// Signal both the timeline semaphore (for internal frame graph) and a binary render-finished semaphore (for WSI present)
	vk::Semaphore render_finished = *m_render_finished_semaphores[*image_index];
	std::array<vk::Semaphore, 2> signal_sems{ *semaphore, render_finished };
	vk::SubmitInfo submit_info{
		.pNext = &timeline_info,
		.waitSemaphoreCount = static_cast<uint32_t>(wait_sems.size()),
		.pWaitSemaphores = wait_sems.data(),
		.pWaitDstStageMask = wait_stages,
		.commandBufferCount = 1,
		.pCommandBuffers = &command_buffer,
		.signalSemaphoreCount = static_cast<uint32_t>(signal_sems.size()),
		.pSignalSemaphores = signal_sems.data()
	};

	// Submit the command buffer to the graphics queue and signal the per-frame fence to ensure safe CB reuse
	m_ve_device.getQueue().submit(submit_info, *m_in_flight_fences[m_current_frame]);

	// Present waits on the binary render-finished semaphore (GPU-side)
	std::array<vk::Semaphore, 1> present_waits{ render_finished };
	const vk::PresentInfoKHR present_info{
		.pNext = nullptr,
		.waitSemaphoreCount = static_cast<uint32_t>(present_waits.size()),
		.pWaitSemaphores = present_waits.data(),
		.swapchainCount = 1,
		.pSwapchains = &*m_swap_chain,
		.pImageIndices = image_index,
		.pResults = nullptr
	};

	// Present the image to the screen
	return m_ve_device.getQueue().presentKHR(present_info);
}

void VeSwapChain::createSwapChain() {
	m_swap_chain_support = m_ve_device.getSwapChainSupport();
	m_surface_format = chooseSwapSurfaceFormat(m_swap_chain_support.formats);
	m_present_mode = chooseSwapPresentMode(m_swap_chain_support.presentModes);
	m_swap_chain_extent = chooseSwapExtent(m_swap_chain_support.capabilities);
	m_swap_chain_image_format = m_surface_format.format;

	// try to use one more than the minimum number of images to improve gpu utilization
	// note that there is no guarantee that we can get that many images, so we need to check against the maximum as well
	uint32_t image_count = m_swap_chain_support.capabilities.minImageCount + 1;
	if (m_swap_chain_support.capabilities.maxImageCount > 0 && image_count > m_swap_chain_support.capabilities.maxImageCount) {
		image_count = m_swap_chain_support.capabilities.maxImageCount;
	}

	vk::SwapchainCreateInfoKHR create_info{
		.sType = vk::StructureType::eSwapchainCreateInfoKHR,
		.pNext = nullptr,
		.flags = {},
		.surface = *m_ve_device.getSurface(),
		.minImageCount = image_count,
		.imageFormat = m_surface_format.format,
		.imageColorSpace = m_surface_format.colorSpace,
		.imageExtent = m_swap_chain_extent,
		.imageArrayLayers = 1, // always 1 unless developing stereoscopic 3D app
		.imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
		.imageSharingMode = vk::SharingMode::eExclusive,
		.queueFamilyIndexCount = 0, // optional
		.pQueueFamilyIndices = nullptr, // optional
		.preTransform = m_swap_chain_support.capabilities.currentTransform,
		.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
		.presentMode = m_present_mode,
		.clipped = VK_TRUE,
		.oldSwapchain = VK_NULL_HANDLE // may be overwritten below if recreating
	};

	// If we are recreating (old_swap_chain retained), provide its handle so the driver
	// can safely migrate resources and release the old one.
	if (m_old_swap_chain != VK_NULL_HANDLE) {
		// Obtain raw handle from vk::raii::SwapchainKHR reference
		create_info.oldSwapchain = *m_old_swap_chain->getSwapChain();
	}

	m_swap_chain = vk::raii::SwapchainKHR(m_ve_device.getDevice(), create_info);
	m_swap_chain_images = m_swap_chain.getImages();
}

void VeSwapChain::createSwapChainImageViews() {
	assert(m_swap_chain_image_views.empty());
	vk::ImageViewCreateInfo create_info{
		.sType = vk::StructureType::eImageViewCreateInfo,
		.pNext = nullptr,
		.flags = {},
		.image = VK_NULL_HANDLE, // will set per iteration
		.viewType = vk::ImageViewType::e2D,
		.format = m_surface_format.format,
		.components = {},
		.subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 }
	};
	for (auto image : m_swap_chain_images) {
		create_info.image = image;
		// call the constructor of vk::raii::ImageView and add it to the vector
		m_swap_chain_image_views.emplace_back(m_ve_device.getDevice(), create_info);
	}
}

// TODO: create a depth image for each swap chain image
void VeSwapChain::createDepthResources() {
	vk::Format depth_format = m_ve_device.findDepthFormat();
	m_depth_image = std::make_unique<VeImage>(
		m_ve_device,
		m_swap_chain_extent.width,
		m_swap_chain_extent.height,
		depth_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eDepthStencilAttachment,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eDepth);

	// transition the depth image to be optimal for a depth attachment using a single-time command buffer
	m_depth_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eDepthStencilAttachmentOptimal,
		{},
		vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eEarlyFragmentTests);
}

// Create 2 semaphores and 1 fence per frame in flight
void VeSwapChain::createSyncObjects() {
	vk::SemaphoreTypeCreateInfo semaphore_type{
		.sType = vk::StructureType::eSemaphoreTypeCreateInfo,
		.pNext = nullptr,
		.semaphoreType = vk::SemaphoreType::eTimeline,
		.initialValue = 0
	};
	vk::SemaphoreCreateInfo timeline_sem_ci{ .pNext = &semaphore_type };
	semaphore = vk::raii::Semaphore(m_ve_device.getDevice(), timeline_sem_ci);
	timeline_value = 0;

	// fences
	m_in_flight_fences.clear();
	vk::FenceCreateInfo fence_info{ .flags = vk::FenceCreateFlagBits::eSignaled };
	for (size_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; i++) {
		m_in_flight_fences.emplace_back(m_ve_device.getDevice(), fence_info);
	}

	// Create per-swapchain-image binary render-finished semaphores used by present
	m_render_finished_semaphores.clear();
	m_render_finished_semaphores.reserve(m_swap_chain_images.size());
	vk::SemaphoreCreateInfo sem_ci{}; // binary semaphore by default
	for (size_t i = 0; i < m_swap_chain_images.size(); ++i) {
		m_render_finished_semaphores.emplace_back(m_ve_device.getDevice(), sem_ci);
	}

	// Create per-frame binary image-available semaphores used by acquire
	m_image_available_semaphores.clear();
	m_image_available_semaphores.reserve(ve::MAX_FRAMES_IN_FLIGHT);
	for (size_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; ++i) {
		m_image_available_semaphores.emplace_back(m_ve_device.getDevice(), sem_ci);
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
		if (available_present_mode == ve::PRESENT_MODE) {
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
		vk::Extent2D actual_extent = m_window_extent;
		actual_extent.width = std::clamp(actual_extent.width,
											capabilities.minImageExtent.width,
											capabilities.maxImageExtent.width);
		actual_extent.height = std::clamp(actual_extent.height,
											capabilities.minImageExtent.height,
											capabilities.maxImageExtent.height);
		return actual_extent;
	}
}

void VeSwapChain::waitForCurrentFence() {
	while (vk::Result::eTimeout ==
			m_ve_device.getDevice().waitForFences(*m_in_flight_fences[m_current_frame], vk::True, UINT64_MAX));
	return;
}

// Reset the fence of the current frame back to unsignaled state
void VeSwapChain::resetCurrentFence() {
	m_ve_device.getDevice().resetFences(*m_in_flight_fences[m_current_frame]);
}

void VeSwapChain::advanceFrame() {
	m_current_frame = (m_current_frame + 1) % ve::MAX_FRAMES_IN_FLIGHT;
}

void VeSwapChain::updateTimelineValues() {
	compute_wait_value = timeline_value;
	compute_signal_value = ++timeline_value;
	graphics_wait_value = compute_signal_value;
	graphics_signal_value = ++timeline_value;
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
		.image = m_swap_chain_images[image_index],
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

float VeSwapChain::getExtentAspectRatio() const {
	return static_cast<float>(m_swap_chain_extent.width) /
			static_cast<float>(m_swap_chain_extent.height);
}

bool VeSwapChain::compareSwapFormats(const VeSwapChain& other) const {
	return (other.m_swap_chain_image_format == m_swap_chain_image_format &&
			other.m_depth_image->getFormat() == m_depth_image->getFormat());
}

} // namespace ve

