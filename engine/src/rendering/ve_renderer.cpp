#include "pch.hpp"
#include "rendering/ve_renderer.hpp"
#include "rendering/ve_frame_info.hpp"
#include "ve_tracy.hpp"

#include <stdexcept>
#include <algorithm>


namespace ve {
// Constructor, initializes swap chain with present mode immediate and command buffers
VeRenderer::VeRenderer(VeDevice& device, VeWindow& window)
	: m_ve_device(device), m_command_manager(device), m_ve_window(window), m_profiler(device) {
	m_ve_swap_chain = std::make_unique<VeSwapChain>(m_ve_device, m_ve_window.getExtent(), m_desired_num_samples, m_present_mode, m_hdr_enabled);
	createViewportResources();

	// Cache formats and create worker thread pool
	m_depth_format = m_ve_device.findDepthFormat();
	m_scene_color_format = getOffscreenImageFormat();
	uint32_t hw = std::thread::hardware_concurrency();
	uint32_t workers = (hw > 2) ? std::min(hw - 1, static_cast<uint32_t>(MAX_RENDER_WORKERS)) : 2u;
	m_thread_pool = std::make_unique<VeThreadPool>(m_command_manager, workers);

#ifdef TRACY_ENABLE
	auto instanceProcAddr = m_ve_device.getInstance().getDispatcher()->vkGetInstanceProcAddr;
	auto deviceProcAddr = m_ve_device.getDevice().getDispatcher()->vkGetDeviceProcAddr;
	bool calibrated = m_ve_device.supportsCalibratedTimestamps();
	auto createCtx = [&](vk::raii::CommandPool& pool, vk::raii::Queue& queue) -> TracyVkCtx {
		vk::CommandBufferAllocateInfo alloc_info{
			.commandPool = *pool,
			.level = vk::CommandBufferLevel::ePrimary,
			.commandBufferCount = 1
		};
		auto bufs = m_ve_device.getDevice().allocateCommandBuffers(alloc_info);
		if (calibrated)
			return TracyVkContextCalibrated(
				*m_ve_device.getInstance(), *m_ve_device.getPhysicalDevice(),
				*m_ve_device.getDevice(), *queue, *bufs[0],
				instanceProcAddr, deviceProcAddr);
		return TracyVkContext(
			*m_ve_device.getInstance(), *m_ve_device.getPhysicalDevice(),
			*m_ve_device.getDevice(), *queue, *bufs[0],
			instanceProcAddr, deviceProcAddr);
	};
	m_tracy_graphics_ctx = createCtx(m_ve_device.getCommandPool(), m_ve_device.getQueue());
	m_tracy_compute_ctx = createCtx(m_ve_device.getComputeCommandPool(), m_ve_device.getComputeQueue());
#endif
}

VeRenderer::~VeRenderer() {
#ifdef TRACY_ENABLE
	m_ve_device.getDevice().waitIdle();
	TracyVkDestroy(m_tracy_graphics_ctx);
	TracyVkDestroy(m_tracy_compute_ctx);
#endif
}

float VeRenderer::getExtentAspectRatio() const {
	auto extent = getExtent();
	return static_cast<float>(extent.width) / static_cast<float>(extent.height);
}
vk::Format VeRenderer::getSwapChainImageFormat() const { return m_ve_swap_chain->getSwapChainImageFormat(); }
vk::ColorSpaceKHR VeRenderer::getSwapChainColorSpace() const { return m_ve_swap_chain->getSwapChainColorSpace(); }
vk::Format VeRenderer::getOffscreenImageFormat() const { return m_ve_swap_chain->getOffscreenImageFormat(); }
size_t VeRenderer::getImageCount() const { return m_ve_swap_chain->getImageCount(); }
vk::Extent2D VeRenderer::getExtent() const {
	if (m_scene_render_extent.width > 0 && m_scene_render_extent.height > 0)
		return m_scene_render_extent;
	return m_ve_swap_chain->getSwapChainExtent();
}
vk::Extent2D VeRenderer::getSwapChainExtent() const { return m_ve_swap_chain->getSwapChainExtent(); }
uint32_t VeRenderer::getCurrentFrame() const {
	assert(m_is_frame_started && "Frame is not in progress");
	return m_ve_swap_chain->getCurrentFrame();
}
vk::raii::CommandBuffer& VeRenderer::getCurrentCommandBuffer() {
	assert(m_is_frame_started && "Frame is not in progress");
	return m_command_manager.getGraphicsPrimary(m_ve_swap_chain->getCurrentFrame());
}
vk::raii::CommandBuffer& VeRenderer::getCurrentComputeCommandBuffer() {
	assert(m_is_frame_started && "Frame is not in progress");
	return m_command_manager.getComputePrimary(m_ve_swap_chain->getCurrentFrame());
}
vk::raii::CommandBuffer& VeRenderer::getCurrentUICommandBuffer() {
	assert(m_is_frame_started && "Frame is not in progress");
	return m_command_manager.getUIPrimary(m_ve_swap_chain->getCurrentFrame());
}

void VeRenderer::recreateSwapChain() {
	// Handle minimized window
	auto extent = m_ve_window.getExtent();
	while (extent.width == 0 || extent.height == 0) {
		extent = m_ve_window.getExtent();
		glfwWaitEvents();
	}

	// Precondition: device must be idle (caller responsibility)
	m_ve_device.assertDeviceIdle();
	extent = m_ve_window.getExtent();
	if (m_ve_swap_chain == nullptr) {
		m_ve_swap_chain = std::make_unique<VeSwapChain>(m_ve_device, extent, m_desired_num_samples, m_present_mode, m_hdr_enabled);
	} else {
		// Transfer ownership of the existing swap chain to a shared_ptr so the new one
		// can safely reference it during recreation.
		std::shared_ptr<VeSwapChain> old_swap_chain{ std::move(m_ve_swap_chain) };
		m_ve_swap_chain = std::make_unique<VeSwapChain>(m_ve_device, extent, m_desired_num_samples, m_present_mode, m_hdr_enabled, old_swap_chain);
	}
	m_swap_chain_needs_recreation = false;
	m_scene_color_format = getOffscreenImageFormat();
	// Re-apply scene render extent if editor mode has a custom extent set
	if (m_scene_render_extent.width > 0 && m_scene_render_extent.height > 0)
		m_ve_swap_chain->resizeOffscreenResources(m_scene_render_extent);
	createViewportResources();
	VE_LOGI("Swap chain recreated: " << extent.width << "x" << extent.height);
}

// Begin a frame; returns true when a frame is started and command buffer can be used
// Returns false if swap chain is out of date
// throws runtime error if acquire fails for other reasons
bool VeRenderer::beginFrame() {
	assert(!m_is_frame_started && "Can't call beginFrame while already in progress");

	// Wait until image is available (measure fence wait time)
	auto fence_start = std::chrono::steady_clock::now();
	{
		ZoneScopedN("Fence Wait");
		m_ve_swap_chain->waitForCurrentFence();
	}
	auto fence_end = std::chrono::steady_clock::now();
	float fence_ms = std::chrono::duration<float, std::chrono::milliseconds::period>(fence_end - fence_start).count();
	m_profiler.recordFenceWait(fence_ms);

	// Acquire an image from the swap chain
	vk::Result result;
	auto acquire_start = std::chrono::steady_clock::now();
	{
		ZoneScopedN("Acquire Image");
		result = m_ve_swap_chain->acquireNextImage(&m_current_image_index);
	}
	auto acquire_end = std::chrono::steady_clock::now();
	float acquire_ms = std::chrono::duration<float, std::chrono::milliseconds::period>(acquire_end - acquire_start).count();
	m_profiler.recordAcquireWait(acquire_ms);
	if (result == vk::Result::eErrorOutOfDateKHR) {
		VE_LOGD("Result of acquireNextImage is eErrorOutOfDateKHR, setting flag.");
		m_swap_chain_needs_recreation = true;
		return false;
	}
	if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}
	if (result == vk::Result::eSuboptimalKHR) {
		VE_LOGD("Result of acquireNextImage is eSuboptimalKHR");
	}

	// frame acquired
	m_is_frame_started = true;
	m_ve_swap_chain->resetCurrentFence();
	m_ve_swap_chain->updateTimelineValues();

	// Begin command buffer for recording commands
	auto& command_buffer = getCurrentCommandBuffer();
	uint32_t frame_index = m_ve_swap_chain->getCurrentFrame();

	// Resolve GPU timing results from the previous use of this frame slot
	m_profiler.beginFrame(frame_index);

	m_command_manager.resetPrimaries(frame_index);
	m_thread_pool->resetFrame(frame_index);
	m_command_manager.resetThreadFrame(m_command_manager.getMainThreadSlot(), frame_index);
	vk::CommandBufferBeginInfo info{
		.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
	};
	command_buffer.begin(info);

	// Also begin compute and UI command buffers
	auto& compute_command_buffer = getCurrentComputeCommandBuffer();
	compute_command_buffer.begin(info);

	auto& ui_command_buffer = getCurrentUICommandBuffer();
	ui_command_buffer.begin(info);

	TracyVkCollect(m_tracy_graphics_ctx, *command_buffer);
	TracyVkCollect(m_tracy_compute_ctx, *compute_command_buffer);

	m_profiler.resetAllQueries(command_buffer, compute_command_buffer, frame_index);

	// Write FRAME_TOTAL start timestamp on graphics queue
	m_profiler.beginGpuTimer(command_buffer, ProfileTimer::FRAME_TOTAL);

	return true;
}

// End scene + UI command buffers, submit both, present, and advance the frame.
void VeRenderer::endFrame() {
	assert(m_is_frame_started && "Can't call endFrame while frame is not in progress");

	auto& scene_cb = getCurrentCommandBuffer();
	auto& ui_cb = getCurrentUICommandBuffer();

	// End scene CB with graphics end timestamp
	m_profiler.endGpuTimer(scene_cb, ProfileTimer::FRAME_TOTAL);
	scene_cb.end();

	// Finalize UI CB: transition swapchain to present, then end
	transitionToPresent(ui_cb);
	ui_cb.end();

	// Submit both CBs in order, then present
	ZoneScopedN("Submit + Present");
	auto result = m_ve_swap_chain->submitAndPresent(*scene_cb, *ui_cb, &m_current_image_index);

	if (result == vk::Result::eErrorOutOfDateKHR) {
		VE_LOGD("Result of present is eErrorOutOfDateKHR, setting flag.");
		m_swap_chain_needs_recreation = true;
	}
	else if (result == vk::Result::eSuboptimalKHR) {
		VE_LOGD("Result of present is eSuboptimalKHR, setting flag.");
		m_swap_chain_needs_recreation = true;
	}
	if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR && result != vk::Result::eErrorOutOfDateKHR) {
		throw std::runtime_error("failed to present swap chain image!");
	}
	// Advance to the next frame
	if (result == vk::Result::eSuccess)
		m_ve_swap_chain->advanceFrame();
	m_is_frame_started = false;
	FrameMark;
}

void VeRenderer::beginDepthPrePass(vk::raii::CommandBuffer& command_buffer,
	bool secondary_contents, bool clear) {
	assert(m_is_frame_started && "Can't begin depth pre-pass while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't begin depth pre-pass on command buffer from a different frame");

	auto extent = getExtent();

	vk::RenderingAttachmentInfo depth_attachment_info = {
		.imageView = *m_ve_swap_chain->getDepthImageView(),
		.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.loadOp = clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
		.storeOp = vk::AttachmentStoreOp::eStore,
		.clearValue = vk::ClearDepthStencilValue{.depth = 1.0f, .stencil = 0}
	};

	// When MSAA is active, resolve depth to a single-sample image at endRendering().
	// Compute shaders (GTAO, shadow mask) read from the resolved depth instead of MSAA depth.
	if (m_ve_swap_chain->hasResolvedDepth()) {
		if (clear) {
			// Prepare resolved depth for resolve target (only on first clear pass).
			vk::ImageMemoryBarrier2 prep{
				.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
				.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
				.dstStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
				.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
				.oldLayout = vk::ImageLayout::eUndefined,
				.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = *m_ve_swap_chain->getResolvedDepthImage(),
				.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
			};
			vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &prep};
			command_buffer.pipelineBarrier2(dep);
		}

		depth_attachment_info.resolveMode = vk::ResolveModeFlagBits::eSampleZero;
		depth_attachment_info.resolveImageView = *m_ve_swap_chain->getResolvedDepthImageView();
		depth_attachment_info.resolveImageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
	}

	vk::RenderingFlags flags{};
	if (secondary_contents)
		flags |= vk::RenderingFlagBits::eContentsSecondaryCommandBuffers;

	vk::RenderingInfo rendering_info = {
		.flags = flags,
		.renderArea = { .offset = { 0, 0 }, .extent = extent },
		.layerCount = 1,
		.colorAttachmentCount = 0,
		.pColorAttachments = nullptr,
		.pDepthAttachment = &depth_attachment_info
	};

	command_buffer.beginRendering(rendering_info);

	// Only set viewport/scissor on the primary when not using secondary contents
	// (secondary CBs must set their own viewport/scissor)
	if (!secondary_contents) {
		command_buffer.setViewport(0, vk::Viewport{
			.x = 0.0f, .y = 0.0f,
			.width = static_cast<float>(extent.width),
			.height = static_cast<float>(extent.height),
			.minDepth = 0.0f, .maxDepth = 1.0f
		});
		command_buffer.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});
	}
}

void VeRenderer::endDepthPrePass(vk::raii::CommandBuffer& command_buffer) {
	assert(m_is_frame_started && "Can't end depth pre-pass while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't end depth pre-pass on command buffer from a different frame");
	command_buffer.endRendering();

	// Barrier: depth writes from pre-pass must be visible before scene pass reads them
	vk::MemoryBarrier2 depth_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
		.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite
	};
	vk::DependencyInfo dep_info{
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &depth_barrier
	};
	command_buffer.pipelineBarrier2(dep_info);
}

// Transitions the resolve target to color_attachment_optimal. Begins dynamic rendering.
void VeRenderer::beginSceneRender(vk::raii::CommandBuffer& command_buffer,
	bool load_depth, bool secondary_contents, bool resolve_msaa) {
	assert(m_is_frame_started && "Can't call beginRender while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't begin render on command buffer from a different frame");

	auto extent = getExtent();

	// Transition resolve target to eColorAttachmentOptimal
	m_ve_swap_chain->transitionResolveTargetLayout(
		command_buffer,
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::AccessFlagBits2::eShaderRead,
		vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::PipelineStageFlagBits2::eFragmentShader,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput
	);

	// Setup dynamic rendering attachments
	vk::RenderingAttachmentInfo color_attachment_info;
	bool msaa_active = m_ve_swap_chain->getSwapChainSampleCount() != vk::SampleCountFlagBits::e1;

	if (msaa_active) {
		color_attachment_info = {
			.sType = vk::StructureType::eRenderingAttachmentInfo,
			.imageView = *m_ve_swap_chain->getColorImageView(),
			.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = resolve_msaa ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearColorValue(0.01f, 0.01f, 0.01f, 1.0f)
		};
		if (resolve_msaa) {
			color_attachment_info.resolveMode = vk::ResolveModeFlagBits::eAverage;
			color_attachment_info.resolveImageView = *m_ve_swap_chain->getResolveTargetImageView();
			color_attachment_info.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		}
	} else {
		color_attachment_info = {
			.sType = vk::StructureType::eRenderingAttachmentInfo,
			.imageView = *m_ve_swap_chain->getResolveTargetImageView(),
			.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearColorValue(0.01f, 0.01f, 0.01f, 1.0f)
		};
	}

	vk::RenderingAttachmentInfo depth_attachment_info = {
		.imageView = *m_ve_swap_chain->getDepthImageView(),
		.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.loadOp = load_depth ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
		.storeOp = vk::AttachmentStoreOp::eStore,
		.clearValue = vk::ClearDepthStencilValue{.depth = 1.0f, .stencil = 0}
	};

	if (m_ve_swap_chain->hasResolvedDepth() && resolve_msaa) {
		depth_attachment_info.resolveMode = vk::ResolveModeFlagBits::eSampleZero;
		depth_attachment_info.resolveImageView = *m_ve_swap_chain->getResolvedDepthImageView();
		depth_attachment_info.resolveImageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
	}

	vk::RenderingFlags flags{};
	if (secondary_contents)
		flags |= vk::RenderingFlagBits::eContentsSecondaryCommandBuffers;

	vk::RenderingInfo rendering_info = {
		.flags = flags,
		.renderArea = { .offset = { 0, 0 }, .extent = extent },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_attachment_info,
		.pDepthAttachment = &depth_attachment_info
	};

	command_buffer.beginRendering(rendering_info);

	if (!secondary_contents) {
		command_buffer.setViewport(0, vk::Viewport{
			.x = 0.0f, .y = 0.0f,
			.width = static_cast<float>(extent.width),
			.height = static_cast<float>(extent.height),
			.minDepth = 0.0f, .maxDepth = 1.0f
		});
		command_buffer.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});
	}
}

void VeRenderer::endSceneRender(vk::raii::CommandBuffer& command_buffer) {
	assert(m_is_frame_started && "Can't call endRender while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't end render on command buffer from a different frame");

	command_buffer.endRendering();

	// Transition the resolve target to eShaderReadOnlyOptimal for post-processing
	m_ve_swap_chain->transitionResolveTargetLayout(
		command_buffer,
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::PipelineStageFlagBits2::eFragmentShader
	);
}

void VeRenderer::beginWboitRender(vk::raii::CommandBuffer& command_buffer) {
	assert(m_is_frame_started && "Can't begin WBOIT render while frame is not in progress");
	auto extent = getExtent();

	// Transition resolved depth to read-only, WBOIT images to color attachment
	vk::ImageMemoryBarrier2 depth_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
		.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead,
		.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *getResolvedDepthImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
	};
	vk::ImageMemoryBarrier2 accum_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
		.oldLayout = vk::ImageLayout::eUndefined,
		.newLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_wboit_accum->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	vk::ImageMemoryBarrier2 revealage_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
		.oldLayout = vk::ImageLayout::eUndefined,
		.newLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_wboit_revealage->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	std::array barriers = {depth_barrier, accum_barrier, revealage_barrier};
	vk::DependencyInfo dep{
		.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
		.pImageMemoryBarriers = barriers.data()
	};
	command_buffer.pipelineBarrier2(dep);

	// Accum: clear to 0 (additive blend accumulates weighted color)
	vk::RenderingAttachmentInfo accum_attachment{
		.imageView = *m_wboit_accum->getImageView(),
		.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.loadOp = vk::AttachmentLoadOp::eClear,
		.storeOp = vk::AttachmentStoreOp::eStore,
		.clearValue = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 0.0f)
	};
	// Revealage: clear to 1 (multiplicative blend: product of (1-alpha) starts at 1)
	vk::RenderingAttachmentInfo revealage_attachment{
		.imageView = *m_wboit_revealage->getImageView(),
		.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.loadOp = vk::AttachmentLoadOp::eClear,
		.storeOp = vk::AttachmentStoreOp::eStore,
		.clearValue = vk::ClearColorValue(1.0f, 0.0f, 0.0f, 0.0f)
	};
	std::array color_attachments = {accum_attachment, revealage_attachment};

	// Depth: read-only (depth test without writes)
	vk::RenderingAttachmentInfo depth_attachment{
		.imageView = *getResolvedDepthImageView(),
		.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal,
		.loadOp = vk::AttachmentLoadOp::eLoad,
		.storeOp = vk::AttachmentStoreOp::eNone,
	};

	vk::RenderingInfo rendering_info{
		.renderArea = {.offset = {0, 0}, .extent = extent},
		.layerCount = 1,
		.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size()),
		.pColorAttachments = color_attachments.data(),
		.pDepthAttachment = &depth_attachment
	};
	command_buffer.beginRendering(rendering_info);

	command_buffer.setViewport(0, vk::Viewport{
		.x = 0.0f, .y = 0.0f,
		.width = static_cast<float>(extent.width),
		.height = static_cast<float>(extent.height),
		.minDepth = 0.0f, .maxDepth = 1.0f
	});
	command_buffer.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});
}

void VeRenderer::endWboitRender(vk::raii::CommandBuffer& command_buffer) {
	command_buffer.endRendering();

	// Transition WBOIT images to shader read for composite pass
	vk::ImageMemoryBarrier2 accum_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_wboit_accum->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	vk::ImageMemoryBarrier2 revealage_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_wboit_revealage->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	std::array barriers = {accum_barrier, revealage_barrier};
	vk::DependencyInfo dep{
		.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
		.pImageMemoryBarriers = barriers.data()
	};
	command_buffer.pipelineBarrier2(dep);
}

void VeRenderer::beginWboitComposite(vk::raii::CommandBuffer& command_buffer) {
	auto extent = getExtent();

	// Transition resolve target back to color attachment for compositing
	m_ve_swap_chain->transitionResolveTargetLayout(
		command_buffer,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::AccessFlagBits2::eShaderRead,
		vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::PipelineStageFlagBits2::eFragmentShader,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput
	);

	vk::RenderingAttachmentInfo color_attachment{
		.imageView = *m_ve_swap_chain->getResolveTargetImageView(),
		.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.loadOp = vk::AttachmentLoadOp::eLoad,
		.storeOp = vk::AttachmentStoreOp::eStore,
	};

	vk::RenderingInfo rendering_info{
		.renderArea = {.offset = {0, 0}, .extent = extent},
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_attachment,
	};
	command_buffer.beginRendering(rendering_info);

	command_buffer.setViewport(0, vk::Viewport{
		.x = 0.0f, .y = 0.0f,
		.width = static_cast<float>(extent.width),
		.height = static_cast<float>(extent.height),
		.minDepth = 0.0f, .maxDepth = 1.0f
	});
	command_buffer.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});
}

void VeRenderer::endWboitComposite(vk::raii::CommandBuffer& command_buffer) {
	command_buffer.endRendering();

	// Transition resolve target back to shader read for bloom/post-process
	m_ve_swap_chain->transitionResolveTargetLayout(
		command_buffer,
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::PipelineStageFlagBits2::eFragmentShader
	);

	// Restore resolved depth to depth attachment for any subsequent passes
	vk::ImageMemoryBarrier2 depth_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
		.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *getResolvedDepthImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
	};
	vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &depth_barrier};
	command_buffer.pipelineBarrier2(dep);
}

void VeRenderer::beginPostProcessRender(vk::raii::CommandBuffer& command_buffer, bool editor_mode) {
	assert(m_is_frame_started && "Can't call beginPostProcessRender while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't begin post-process on command buffer from a different frame");

	if (editor_mode && m_viewport_image) {
		// Editor mode: render to viewport image
		auto vp_extent = m_viewport_image->getExtent2D();

		m_viewport_image->transitionImageLayout(
			command_buffer,
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eColorAttachmentOptimal,
			{},
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput
		);

		vk::RenderingAttachmentInfo color_attachment_info = {
			.sType = vk::StructureType::eRenderingAttachmentInfo,
			.imageView = *m_viewport_image->getImageView(),
			.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f)
		};

		vk::RenderingInfo rendering_info = {
			.renderArea = { .offset = { 0, 0 }, .extent = vp_extent },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_attachment_info
		};

		command_buffer.beginRendering(rendering_info);
		command_buffer.setViewport(0, vk::Viewport{
			.x = 0.0f, .y = 0.0f,
			.width = static_cast<float>(vp_extent.width),
			.height = static_cast<float>(vp_extent.height),
			.minDepth = 0.0f, .maxDepth = 1.0f
		});
		command_buffer.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = vp_extent});
	} else {
		// Fullscreen mode: render to swapchain (current behavior)
		auto extent = m_ve_swap_chain->getSwapChainExtent();

		m_ve_swap_chain->transitionImageLayout(
			command_buffer,
			m_current_image_index,
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eColorAttachmentOptimal,
			{},
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput
		);

		vk::RenderingAttachmentInfo color_attachment_info = {
			.sType = vk::StructureType::eRenderingAttachmentInfo,
			.imageView = *m_ve_swap_chain->getSwapChainImageViews()[m_current_image_index],
			.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f)
		};

		vk::RenderingInfo rendering_info = {
			.renderArea = { .offset = { 0, 0 }, .extent = extent },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_attachment_info
		};

		command_buffer.beginRendering(rendering_info);
		command_buffer.setViewport(0, vk::Viewport{
			.x = 0.0f, .y = 0.0f,
			.width = static_cast<float>(extent.width),
			.height = static_cast<float>(extent.height),
			.minDepth = 0.0f, .maxDepth = 1.0f
		});
		command_buffer.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});
	}
}

void VeRenderer::endPostProcessRender(vk::raii::CommandBuffer& command_buffer, bool editor_mode) {
	assert(m_is_frame_started && "Can't call endPostProcessRender while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't end post-process on command buffer from a different frame");

	command_buffer.endRendering();

	if (editor_mode && m_viewport_image) {
		// Transition viewport image to shader read for ImGui sampling
		m_viewport_image->transitionImageLayout(
			command_buffer,
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eFragmentShader
		);
	}
}

void VeRenderer::beginUIRecording(bool editor_mode) {
	assert(m_is_frame_started && "Can't call beginUIRecording while frame is not in progress");

	auto& ui_cb = getCurrentUICommandBuffer();

	if (editor_mode) {
		// Editor mode: scene rendered to viewport image, swapchain is untouched.
		// Transition swapchain from eUndefined to eColorAttachmentOptimal for ImGui.
		m_ve_swap_chain->transitionImageLayout(
			ui_cb,
			m_current_image_index,
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eColorAttachmentOptimal,
			{},
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput
		);
	} else {
		// Fullscreen mode: scene CB wrote to swapchain via post-process.
		// Memory barrier ensures scene writes are visible before UI renders on top.
		vk::MemoryBarrier2 barrier{
			.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite
		};
		vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
		ui_cb.pipelineBarrier2(dep);
	}
}

void VeRenderer::recreateWboitImages() {
	auto extent = getExtent();
	if (extent.width == 0 || extent.height == 0)
		return;

	m_wboit_accum = std::make_unique<VeImage>(
		m_ve_device,
		extent.width, extent.height,
		vk::SampleCountFlagBits::e1,
		vk::Format::eR16G16B16A16Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1
	);

	m_wboit_revealage = std::make_unique<VeImage>(
		m_ve_device,
		extent.width, extent.height,
		vk::SampleCountFlagBits::e1,
		vk::Format::eR16Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1
	);

	m_wboit_accum->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		{},
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader
	);
	m_wboit_revealage->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		{},
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader
	);
}

void VeRenderer::createViewportResources() {
	auto extent = m_ve_swap_chain->getSwapChainExtent();
	auto format = m_ve_swap_chain->getSwapChainImageFormat();

	recreateWboitImages();

	m_viewport_image = std::make_unique<VeImage>(
		m_ve_device,
		extent.width, extent.height,
		vk::SampleCountFlagBits::e1,
		format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1
	);

	// Transition to SHADER_READ_ONLY so it's safe to sample before first render
	m_viewport_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		{},
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader
	);

	if (!*m_viewport_sampler) {
		vk::SamplerCreateInfo sampler_info{
			.magFilter = vk::Filter::eLinear,
			.minFilter = vk::Filter::eLinear,
			.mipmapMode = vk::SamplerMipmapMode::eLinear,
			.addressModeU = vk::SamplerAddressMode::eClampToEdge,
			.addressModeV = vk::SamplerAddressMode::eClampToEdge,
			.addressModeW = vk::SamplerAddressMode::eClampToEdge,
			.mipLodBias = 0.0f,
			.anisotropyEnable = vk::False,
			.maxAnisotropy = 1.0f,
			.compareEnable = vk::False,
			.compareOp = vk::CompareOp::eAlways,
			.minLod = 0.0f,
			.maxLod = 1.0f,
			.borderColor = vk::BorderColor::eIntOpaqueBlack,
			.unnormalizedCoordinates = vk::False
		};
		m_viewport_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
	}
}

VkImageView VeRenderer::getViewportImageView() const {
	return m_viewport_image ? static_cast<VkImageView>(*m_viewport_image->getImageView()) : VK_NULL_HANDLE;
}

VkSampler VeRenderer::getViewportSampler() const {
	return *m_viewport_sampler ? static_cast<VkSampler>(*m_viewport_sampler) : VK_NULL_HANDLE;
}

void VeRenderer::resizeSceneRender(uint32_t w, uint32_t h) {
	if (w == 0 || h == 0)
		return;
	m_scene_render_extent = vk::Extent2D{w, h};
	m_ve_swap_chain->resizeOffscreenResources({w, h});
	recreateWboitImages();
}

void VeRenderer::resetSceneRenderExtent() {
	m_scene_render_extent = vk::Extent2D{0, 0};
	m_ve_swap_chain->resizeOffscreenResources(m_ve_swap_chain->getSwapChainExtent());
	recreateWboitImages();
}

void VeRenderer::resizeViewportImage(uint32_t width, uint32_t height) {
	if (width == 0 || height == 0)
		return;
	if (m_viewport_image && m_viewport_image->getWidth() == width && m_viewport_image->getHeight() == height)
		return;

	// Precondition: device must be idle
	m_ve_device.assertDeviceIdle();
	auto format = m_ve_swap_chain->getSwapChainImageFormat();

	m_viewport_image = std::make_unique<VeImage>(
		m_ve_device,
		width, height,
		vk::SampleCountFlagBits::e1,
		format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1
	);

	// Transition to SHADER_READ_ONLY so it's safe to sample before first render
	m_viewport_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		{},
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader
	);
}

void VeRenderer::transitionToPresent(vk::raii::CommandBuffer& command_buffer) {
	assert(m_is_frame_started && "Can't call transitionToPresent while frame is not in progress");
	// After all rendering, transition swap chain image to presentation
	m_ve_swap_chain->transitionImageLayout(
		command_buffer,
		m_current_image_index,
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageLayout::ePresentSrcKHR,
		vk::AccessFlagBits2::eColorAttachmentWrite,
		{},
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::PipelineStageFlagBits2::eBottomOfPipe
	);
}

// Writes compute end timestamp, ends the command buffer, and submits to the compute queue.
// Should be called between beginFrame() and endFrame().
void VeRenderer::submitCompute(vk::raii::CommandBuffer& compute_command_buffer) {
	assert(m_is_frame_started && "Can't call submitCompute while frame is not in progress");
	assert(&compute_command_buffer == &getCurrentComputeCommandBuffer() && "Can't submit compute on command buffer from a different frame");

	m_profiler.endGpuTimer(compute_command_buffer, ProfileTimer::COMPUTE_TOTAL);
	compute_command_buffer.end();

	m_ve_swap_chain->submitComputeWork(*compute_command_buffer);
}

// --- App-facing wrappers ---

std::vector<int> VeRenderer::getAvailableSampleCounts() const {
	std::vector<int> counts;
	counts.push_back(1);
	vk::SampleCountFlagBits max_samples = m_ve_device.getSampleCount();
	for (int i = 2; i <= 64; i *= 2)
		if (static_cast<vk::SampleCountFlagBits>(i) <= max_samples)
			counts.push_back(i);
	return counts;
}

void VeRenderer::setSampleCountInt(int count) {
	setSampleCount(static_cast<vk::SampleCountFlagBits>(count));
}

HDRColorMode VeRenderer::getHDRColorMode() const {
	if (!m_hdr_enabled)
		return HDRColorMode::SDR;
	auto cs = getSwapChainColorSpace();
	if (cs == vk::ColorSpaceKHR::eExtendedSrgbLinearEXT)
		return HDRColorMode::SCRGB;
	if (cs == vk::ColorSpaceKHR::eHdr10St2084EXT)
		return HDRColorMode::HDR10_PQ;
	return HDRColorMode::SDR;
}

const char* VeRenderer::getHDRColorModeString() const {
	switch (getHDRColorMode()) {
		case HDRColorMode::SCRGB:    return "Extended sRGB (scRGB)";
		case HDRColorMode::HDR10_PQ: return "HDR10 (PQ)";
		default:                     return "";
	}
}

vk::CommandBufferInheritanceRenderingInfo VeRenderer::getSceneInheritanceInfo() const {
	return vk::CommandBufferInheritanceRenderingInfo{
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &m_scene_color_format,
		.depthAttachmentFormat = m_depth_format,
		.rasterizationSamples = m_desired_num_samples
	};
}

vk::CommandBufferInheritanceRenderingInfo VeRenderer::getDepthPrepassInheritanceInfo() const {
	return vk::CommandBufferInheritanceRenderingInfo{
		.colorAttachmentCount = 0,
		.depthAttachmentFormat = m_depth_format,
		.rasterizationSamples = m_desired_num_samples
	};
}

}
