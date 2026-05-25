#include "pch.hpp"
#include "vulkan/ve_swap_chain.hpp"
#include "vulkan/ve_debug_utils.hpp"

#include <array>
#include <limits>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cassert>

namespace ve {

VeSwapChain::VeSwapChain(VeDevice& device, vk::Extent2D window_extent, vk::SampleCountFlagBits desired_num_samples, vk::PresentModeKHR present_mode, bool hdr_enabled)
	: m_ve_device(device), m_window_extent(window_extent), m_present_mode(present_mode), m_desired_num_samples(desired_num_samples), m_hdr_enabled(hdr_enabled) {
	// Clamp desired samples to device max
	m_desired_num_samples = desired_num_samples > m_ve_device.getSampleCount() ? m_ve_device.getSampleCount() : desired_num_samples;
	init();
}

VeSwapChain::VeSwapChain(VeDevice& device, vk::Extent2D window_extent, vk::SampleCountFlagBits desired_num_samples, vk::PresentModeKHR present_mode, bool hdr_enabled, std::shared_ptr<VeSwapChain> old_swap_chain)
	: m_ve_device(device), m_window_extent(window_extent), m_present_mode(present_mode), m_desired_num_samples(desired_num_samples), m_hdr_enabled(hdr_enabled), m_old_swap_chain(old_swap_chain) {

	m_desired_num_samples = desired_num_samples > m_ve_device.getSampleCount() ? m_ve_device.getSampleCount() : desired_num_samples;
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
	m_offscreen_extent = m_swap_chain_extent;
	createSwapChainImageViews();
	createColorResources();
	createDepthResources();
	createSyncObjects();

	// Naming for debugging
	for (size_t i = 0; i < m_swap_chain_images.size(); ++i) {
		auto name = "SwapChain Image " + std::to_string(i);
		setDebugName(m_ve_device, vk::ObjectType::eImage,
			reinterpret_cast<uint64_t>(static_cast<VkImage>(m_swap_chain_images[i])), name.c_str());
		setDebugName(m_ve_device, m_swap_chain_image_views[i], name.c_str());
	}
	if (m_depth_image)
		m_depth_image->setDebugName("Depth Image");
	if (m_color_image)
		m_color_image->setDebugName("MSAA Color Image");
	if (m_resolve_target_image)
		m_resolve_target_image->setDebugName("Resolve Target");
	if (m_resolved_depth_image)
		m_resolved_depth_image->setDebugName("Resolved Depth");
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

void VeSwapChain::submitCompute(vk::CommandBuffer command_buffer) {
	// Wait for the previous frame's compute submit to finish.
	uint64_t wait_value = m_compute_signal_value - 1;
	bool has_previous = wait_value > 0;
	vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eComputeShader;

	const vk::TimelineSemaphoreSubmitInfo timeline_info{
		.sType = vk::StructureType::eTimelineSemaphoreSubmitInfo,
		.pNext = nullptr,
		.waitSemaphoreValueCount = has_previous ? 1u : 0u,
		.pWaitSemaphoreValues = has_previous ? &wait_value : nullptr,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &m_compute_signal_value,
	};
	vk::SubmitInfo submit_info{
		.pNext = &timeline_info,
		.waitSemaphoreCount = has_previous ? 1u : 0u,
		.pWaitSemaphores = has_previous ? &*m_frame_timeline : nullptr,
		.pWaitDstStageMask = has_previous ? &wait_stage : nullptr,
		.commandBufferCount = 1,
		.pCommandBuffers = &command_buffer,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &*m_frame_timeline,
	};

	m_ve_device.getComputeQueue().submit(submit_info, nullptr);
}

// Single-submit scene fallback: scene_cb (everything: cull, depth, shadows, render, post)
// + ui_cb in one submit.
vk::Result VeSwapChain::submitSceneAndPresent(
	vk::CommandBuffer scene_cb, vk::CommandBuffer ui_cb, uint32_t* image_index) {
	vk::PipelineStageFlags wait_stages[2] = {
		vk::PipelineStageFlagBits::eColorAttachmentOutput,
		vk::PipelineStageFlagBits::eDrawIndirect
			| vk::PipelineStageFlagBits::eVertexInput
			| vk::PipelineStageFlagBits::eFragmentShader,
	};

	std::array<uint64_t, 2> wait_values{ uint64_t{0}, m_compute_signal_value };
	uint64_t signal_value{0};
	vk::TimelineSemaphoreSubmitInfo timeline_info{
		.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size()),
		.pWaitSemaphoreValues = wait_values.data(),
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &signal_value,
	};

	std::array<vk::Semaphore, 2> wait_sems{ *m_image_available_semaphores[m_current_frame], *m_frame_timeline };
	vk::Semaphore render_finished = *m_render_finished_semaphores[*image_index];
	std::array<vk::CommandBuffer, 2> cbs{ scene_cb, ui_cb };
	vk::SubmitInfo submit_info{
		.pNext = &timeline_info,
		.waitSemaphoreCount = static_cast<uint32_t>(wait_sems.size()),
		.pWaitSemaphores = wait_sems.data(),
		.pWaitDstStageMask = wait_stages,
		.commandBufferCount = static_cast<uint32_t>(cbs.size()),
		.pCommandBuffers = cbs.data(),
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &render_finished,
	};

	m_ve_device.getQueue().submit(submit_info, *m_in_flight_fences[m_current_frame]);

	const vk::PresentInfoKHR present_info{
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &render_finished,
		.swapchainCount = 1,
		.pSwapchains = &*m_swap_chain,
		.pImageIndices = image_index,
	};

	try {
		return m_ve_device.getQueue().presentKHR(present_info);
	} catch (const vk::OutOfDateKHRError& e) {
		VE_LOGD("PresentKHR threw eErrorOutOfDateKHR" << e.what());
		return vk::Result::eErrorOutOfDateKHR;
	}
}

// Submit only a UI command buffer (used by the no-scene editor path).
// No timeline involvement: waits on image_available, signals render_finished + fence, presents.
vk::Result VeSwapChain::submitUIOnly(vk::CommandBuffer ui_cb, uint32_t* image_index) {
	vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
	vk::Semaphore wait_sem = *m_image_available_semaphores[m_current_frame];
	vk::Semaphore render_finished = *m_render_finished_semaphores[*image_index];

	vk::SubmitInfo submit_info{
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &wait_sem,
		.pWaitDstStageMask = &wait_stage,
		.commandBufferCount = 1,
		.pCommandBuffers = &ui_cb,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &render_finished,
	};

	m_ve_device.getQueue().submit(submit_info, *m_in_flight_fences[m_current_frame]);

	const vk::PresentInfoKHR present_info{
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &render_finished,
		.swapchainCount = 1,
		.pSwapchains = &*m_swap_chain,
		.pImageIndices = image_index,
	};

	try {
		return m_ve_device.getQueue().presentKHR(present_info);
	} catch (const vk::OutOfDateKHRError& e) {
		VE_LOGD("PresentKHR threw eErrorOutOfDateKHR" << e.what());
		return vk::Result::eErrorOutOfDateKHR;
	}
}

void VeSwapChain::createSwapChain() {
	m_swap_chain_support = m_ve_device.getSwapChainSupport();
	m_surface_format = chooseSwapSurfaceFormat(m_swap_chain_support.formats);
	m_present_mode = chooseSwapPresentMode(m_swap_chain_support.presentModes);
	m_swap_chain_extent = chooseSwapExtent(m_swap_chain_support.capabilities);
	m_swap_chain_image_format = m_surface_format.format;
	m_offscreen_image_format = m_ve_device.findSupportedFormat(
		{vk::Format::eR16G16B16A16Sfloat, vk::Format::eR32G32B32A32Sfloat},
		vk::ImageTiling::eOptimal,
		vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eSampledImage
	);
	VE_LOGI("Offscreen format: " << vk::to_string(m_offscreen_image_format));
	VE_LOGI("Color format: " << vk::to_string(m_surface_format.format));
	VE_LOGI("Color space: " << vk::to_string(m_surface_format.colorSpace));

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
		.surface = **m_ve_device.getSurface(),
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

void VeSwapChain::createColorResources() {
	m_color_image = std::make_unique<VeImage>(
		m_ve_device,
		m_offscreen_extent.width,
		m_offscreen_extent.height,
		m_desired_num_samples,
		m_offscreen_image_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransientAttachment,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false,
		1);

	m_color_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eColorAttachmentOptimal,
		{},
		vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, // src stage
		vk::PipelineStageFlagBits2::eColorAttachmentOutput // dst stage
	);

	m_resolve_target_image = std::make_unique<VeImage>(
		m_ve_device,
		m_offscreen_extent.width,
		m_offscreen_extent.height,
		vk::SampleCountFlagBits::e1,
		m_offscreen_image_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false,
		1);

	m_resolve_target_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		{},
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader
	);

	VE_LOGD("Color resources created (including resolve target)");
}

void VeSwapChain::createDepthResources() {
	vk::Format depth_format = m_ve_device.findDepthFormat();
	m_depth_image = std::make_unique<VeImage>(
		m_ve_device,
		m_offscreen_extent.width,
		m_offscreen_extent.height,
		m_desired_num_samples,
		depth_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eDepth,
		false,
		1
	);

	// transition the depth image once
	m_depth_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eDepthStencilAttachmentOptimal,
		{},
		vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, // src stage
		vk::PipelineStageFlagBits2::eEarlyFragmentTests // dst stage
	);

	// Create single-sample resolve target when MSAA is active.
	// Resolved during depth prepass via VkRenderingAttachmentInfo::resolveMode.
	// Compute shaders can read from this instead of MSAA depth.
	if (m_desired_num_samples != vk::SampleCountFlagBits::e1) {
		m_resolved_depth_image = std::make_unique<VeImage>(
			m_ve_device,
			m_offscreen_extent.width,
			m_offscreen_extent.height,
			vk::SampleCountFlagBits::e1,
			depth_format,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eDepth,
			false, 1
		);
		m_resolved_depth_image->transitionImageLayout(
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eDepthAttachmentOptimal,
			{},
			vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eEarlyFragmentTests
		);
	} else {
		m_resolved_depth_image.reset();
	}

	VE_LOGD("Depth resource created");
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
	m_frame_timeline = vk::raii::Semaphore(m_ve_device.getDevice(), timeline_sem_ci);
	m_frame_timeline_value = 0;
	setDebugName(m_ve_device, m_frame_timeline, "Frame Timeline Semaphore");

	// fences
	m_in_flight_fences.clear();
	vk::FenceCreateInfo fence_info{ .flags = vk::FenceCreateFlagBits::eSignaled };
	for (size_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; i++) {
		m_in_flight_fences.emplace_back(m_ve_device.getDevice(), fence_info);
		auto name = "In-Flight Fence [" + std::to_string(i) + "]";
		setDebugName(m_ve_device, m_in_flight_fences.back(), name.c_str());
	}

	// Create per-swapchain-image binary render-finished semaphores used by present
	m_render_finished_semaphores.clear();
	m_render_finished_semaphores.reserve(m_swap_chain_images.size());
	vk::SemaphoreCreateInfo sem_ci{}; // binary semaphore by default
	for (size_t i = 0; i < m_swap_chain_images.size(); ++i) {
		m_render_finished_semaphores.emplace_back(m_ve_device.getDevice(), sem_ci);
		auto name = "Render Finished Semaphore [" + std::to_string(i) + "]";
		setDebugName(m_ve_device, m_render_finished_semaphores.back(), name.c_str());
	}

	// Create per-frame binary image-available semaphores used by acquire
	m_image_available_semaphores.clear();
	m_image_available_semaphores.reserve(ve::MAX_FRAMES_IN_FLIGHT);
	for (size_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; ++i) {
		m_image_available_semaphores.emplace_back(m_ve_device.getDevice(), sem_ci);
		auto name = "Image Available Semaphore [" + std::to_string(i) + "]";
		setDebugName(m_ve_device, m_image_available_semaphores.back(), name.c_str());
	}
}

// Choose the surface format, which is a combination of color depth and color space.
// Prefer 8-bit BGRA and SRGB nonlinear color space for now.
vk::SurfaceFormatKHR VeSwapChain::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available_formats) {
	// print all available formats and color spaces

	if (m_ve_device.hasHdrColorSpaceExtension() && m_hdr_enabled) {
#if defined(__APPLE__)
		// macOS: Extended SRGB Linear
		std::vector<vk::ColorSpaceKHR> preferred_spaces = {
			vk::ColorSpaceKHR::eExtendedSrgbLinearEXT,
			vk::ColorSpaceKHR::eHdr10St2084EXT
		};
#else
		// Windows/Linux: HDR10 (PQ)
		std::vector<vk::ColorSpaceKHR> preferred_spaces = {
			vk::ColorSpaceKHR::eHdr10St2084EXT,
			vk::ColorSpaceKHR::eExtendedSrgbLinearEXT
		};
#endif

		for (auto space : preferred_spaces) {
			for (const auto& format : available_formats) {
				if (space == vk::ColorSpaceKHR::eHdr10St2084EXT) {
					if ((format.format == vk::Format::eA2B10G10R10UnormPack32 || format.format == vk::Format::eR16G16B16A16Sfloat) &&
						format.colorSpace == space) {
						VE_LOGI("Selected HDR (PQ) Swapchain Format: " << vk::to_string(format.format) << " ColorSpace: " << vk::to_string(format.colorSpace));
						return format;
					}
				} else if (space == vk::ColorSpaceKHR::eExtendedSrgbLinearEXT) {
					if (format.format == vk::Format::eR16G16B16A16Sfloat && format.colorSpace == space) {
						VE_LOGI("Selected HDR (Linear) Swapchain Format: " << vk::to_string(format.format) << " ColorSpace: " << vk::to_string(format.colorSpace));
						return format;
					}
				}
			}
		}
	}

	// Priority 2: Standard SRGB
	for (const auto& available_format : available_formats) {
		if (available_format.format == vk::Format::eB8G8R8A8Srgb &&
			available_format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
			VE_LOGI("Selected SDR Swapchain Format: " << vk::to_string(available_format.format) << " ColorSpace: " << vk::to_string(available_format.colorSpace));
			return available_format;
		}
	}

	VE_LOGW("Could not find preferred swapchain format, falling back to: " << vk::to_string(available_formats[0].format));
	return available_formats[0];
}

// Choose the presentation mode, which is how images are presented to the screen.
// Mailbox is the lowest latency non-tearing mode, but may not be available.
// Fifo is always available, but may have higher latency.
// Immediate may be available, and is lowest latency, but may have tearing.
vk::PresentModeKHR VeSwapChain::chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& available_present_modes) {
	for (const auto& available_present_mode : available_present_modes) {
		if (available_present_mode == m_present_mode) {
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
    constexpr uint64_t timeout_ns = 3'000'000'000ULL; // 3 seconds
    vk::Result result;
    while ((result = m_ve_device.getDevice().waitForFences(
            *m_in_flight_fences[m_current_frame], vk::True, timeout_ns))
           == vk::Result::eTimeout) {
        VE_LOGW("Fence wait timeout on frame " << m_current_frame
                 << " (timeline=" << m_frame_timeline_value << ")");
    }
}

// Reset the fence of the current frame back to unsignaled state
void VeSwapChain::resetCurrentFence() {
	m_ve_device.getDevice().resetFences(*m_in_flight_fences[m_current_frame]);
}

void VeSwapChain::advanceFrame() {
	m_current_frame = (m_current_frame + 1) % ve::MAX_FRAMES_IN_FLIGHT;
}

void VeSwapChain::beginTimelineFrame() {
	m_compute_signal_value = ++m_frame_timeline_value;
}

void VeSwapChain::prepareSubmitValues(bool depth_compute_follows) {
	m_pre_swap_signal_value = ++m_frame_timeline_value;
	if (depth_compute_follows) {
		m_depth_compute_signal_value = ++m_frame_timeline_value;
		m_swap_wait_value = m_depth_compute_signal_value;
	} else {
		m_swap_wait_value = m_pre_swap_signal_value;
	}
}

void VeSwapChain::submitPreSwapGraphics(vk::CommandBuffer cb) {
	// Waits on compute + image_available; signals pre_swap.
	vk::PipelineStageFlags wait_stages[2] = {
		vk::PipelineStageFlagBits::eColorAttachmentOutput,
		vk::PipelineStageFlagBits::eDrawIndirect
			| vk::PipelineStageFlagBits::eVertexInput
			| vk::PipelineStageFlagBits::eFragmentShader,
	};

	std::array<uint64_t, 2> wait_values{ uint64_t{0}, m_compute_signal_value };
	vk::TimelineSemaphoreSubmitInfo timeline_info{
		.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size()),
		.pWaitSemaphoreValues = wait_values.data(),
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &m_pre_swap_signal_value,
	};

	std::array<vk::Semaphore, 2> wait_sems{ *m_image_available_semaphores[m_current_frame], *m_frame_timeline };
	vk::SubmitInfo submit_info{
		.pNext = &timeline_info,
		.waitSemaphoreCount = static_cast<uint32_t>(wait_sems.size()),
		.pWaitSemaphores = wait_sems.data(),
		.pWaitDstStageMask = wait_stages,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &*m_frame_timeline,
	};

	m_ve_device.getQueue().submit(submit_info, nullptr);
}

void VeSwapChain::submitShadowGraphics(vk::CommandBuffer cb) {
	// Same-queue ordering: runs after pre_swap_graphics, before swap_graphics.
	vk::SubmitInfo submit_info{
		.commandBufferCount = 1,
		.pCommandBuffers = &cb,
	};
	m_ve_device.getQueue().submit(submit_info, nullptr);
}

void VeSwapChain::submitDepthCompute(vk::CommandBuffer cb) {
	// Waits on pre_swap (for depth output), signals depth_compute.
	vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eComputeShader;

	vk::TimelineSemaphoreSubmitInfo timeline_info{
		.waitSemaphoreValueCount = 1,
		.pWaitSemaphoreValues = &m_pre_swap_signal_value,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &m_depth_compute_signal_value,
	};

	vk::SubmitInfo submit_info{
		.pNext = &timeline_info,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &*m_frame_timeline,
		.pWaitDstStageMask = &wait_stage,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &*m_frame_timeline,
	};

	m_ve_device.getComputeQueue().submit(submit_info, nullptr);
}

vk::Result VeSwapChain::submitSwapGraphicsAndPresent(
	vk::CommandBuffer scene_cb, vk::CommandBuffer ui_cb, uint32_t* image_index) {
	// Waits on the last pre-swap timeline value
	vk::PipelineStageFlags wait_stage =
		vk::PipelineStageFlagBits::eDrawIndirect
		| vk::PipelineStageFlagBits::eVertexInput
		| vk::PipelineStageFlagBits::eFragmentShader;

	uint64_t signal_value{0};
	vk::TimelineSemaphoreSubmitInfo timeline_info{
		.waitSemaphoreValueCount = 1,
		.pWaitSemaphoreValues = &m_swap_wait_value,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &signal_value,
	};

	vk::Semaphore render_finished = *m_render_finished_semaphores[*image_index];
	std::array<vk::CommandBuffer, 2> cbs = {scene_cb, ui_cb};
	vk::SubmitInfo submit_info{
		.pNext = &timeline_info,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &*m_frame_timeline,
		.pWaitDstStageMask = &wait_stage,
		.commandBufferCount = static_cast<uint32_t>(cbs.size()),
		.pCommandBuffers = cbs.data(),
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &render_finished,
	};

	m_ve_device.getQueue().submit(submit_info, *m_in_flight_fences[m_current_frame]);

	const vk::PresentInfoKHR present_info{
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &render_finished,
		.swapchainCount = 1,
		.pSwapchains = &*m_swap_chain,
		.pImageIndices = image_index,
	};

	try {
		return m_ve_device.getQueue().presentKHR(present_info);
	} catch (const vk::OutOfDateKHRError& e) {
		VE_LOGD("PresentKHR threw eErrorOutOfDateKHR" << e.what());
		return vk::Result::eErrorOutOfDateKHR;
	}
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

void VeSwapChain::transitionResolveTargetLayout(
		vk::raii::CommandBuffer& command_buffer,
		vk::ImageLayout old_layout,
		vk::ImageLayout new_layout,
		vk::AccessFlags2 src_access_mask,
		vk::AccessFlags2 dst_access_mask,
		vk::PipelineStageFlags2 src_stage_mask,
		vk::PipelineStageFlags2 dst_stage_mask) {

	vk::ImageMemoryBarrier2 barrier = {
		.srcStageMask = src_stage_mask,
		.srcAccessMask = src_access_mask,
		.dstStageMask = dst_stage_mask,
		.dstAccessMask = dst_access_mask,
		.oldLayout = old_layout,
		.newLayout = new_layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_resolve_target_image->getImage(),
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

void VeSwapChain::resizeOffscreenResources(vk::Extent2D extent) {
	// Precondition: device must be idle
	m_ve_device.assertDeviceIdle();
	m_offscreen_extent = extent;
	createColorResources();
	createDepthResources();
	if (m_depth_image)
		m_depth_image->setDebugName("Depth Image");
	if (m_color_image)
		m_color_image->setDebugName("MSAA Color Image");
	if (m_resolve_target_image)
		m_resolve_target_image->setDebugName("Resolve Target");
	if (m_resolved_depth_image)
		m_resolved_depth_image->setDebugName("Resolved Depth");
	VE_LOGI("Offscreen resources resized to " << extent.width << "x" << extent.height);
}

float VeSwapChain::getExtentAspectRatio() const {
	return static_cast<float>(m_swap_chain_extent.width) /
			static_cast<float>(m_swap_chain_extent.height);
}

bool VeSwapChain::compareSwapFormats(const VeSwapChain& other) const {
	return (other.m_swap_chain_image_format == m_swap_chain_image_format &&
			other.m_offscreen_image_format == m_offscreen_image_format &&
			other.m_depth_image->getFormat() == m_depth_image->getFormat());
}

} // namespace ve

