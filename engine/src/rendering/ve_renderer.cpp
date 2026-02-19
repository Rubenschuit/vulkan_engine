#include "pch.hpp"
#include "rendering/ve_renderer.hpp"

#include <stdexcept>
#include <algorithm>


namespace ve {
// Constructor, initializes swap chain with present mode immediate and command buffers
VeRenderer::VeRenderer(VeDevice& device, VeWindow& window) : m_ve_device(device), m_ve_window(window) {
	m_ve_swap_chain = std::make_unique<VeSwapChain>(m_ve_device, m_ve_window.getExtent(), m_desired_num_samples, m_present_mode, m_hdr_enabled);
	createCommandBuffers();

	// Create query pool for GPU timing (4 per frame: compute start/end, graphics start/end)
	vk::QueryPoolCreateInfo query_pool_info{
		.sType = vk::StructureType::eQueryPoolCreateInfo,
		.queryType = vk::QueryType::eTimestamp,
		.queryCount = 4 * ve::MAX_FRAMES_IN_FLIGHT
	};
	m_query_pool = vk::raii::QueryPool(m_ve_device.getDevice(), query_pool_info);
	m_query_active.resize(ve::MAX_FRAMES_IN_FLIGHT, false);
}

VeRenderer::~VeRenderer() {}

float VeRenderer::getExtentAspectRatio() const { return m_ve_swap_chain->getExtentAspectRatio(); }
vk::Format VeRenderer::getSwapChainImageFormat() const { return m_ve_swap_chain->getSwapChainImageFormat(); }
vk::ColorSpaceKHR VeRenderer::getSwapChainColorSpace() const { return m_ve_swap_chain->getSwapChainColorSpace(); }
vk::Format VeRenderer::getOffscreenImageFormat() const { return m_ve_swap_chain->getOffscreenImageFormat(); }
size_t VeRenderer::getImageCount() const { return m_ve_swap_chain->getImageCount(); }
vk::Extent2D VeRenderer::getExtent() const { return m_ve_swap_chain->getSwapChainExtent(); }
uint32_t VeRenderer::getCurrentFrame() const {
	assert(m_is_frame_started && "Frame is not in progress");
	return m_ve_swap_chain->getCurrentFrame();
}
vk::raii::CommandBuffer& VeRenderer::getCurrentCommandBuffer() {
	assert(m_is_frame_started && "Frame is not in progress");
	return m_command_buffers[m_ve_swap_chain->getCurrentFrame()];
}
vk::raii::CommandBuffer& VeRenderer::getCurrentComputeCommandBuffer() {
	assert(m_is_frame_started && "Frame is not in progress");
	return m_compute_command_buffers[m_ve_swap_chain->getCurrentFrame()];
}

void VeRenderer::createCommandBuffers() {
	vk::CommandBufferAllocateInfo alloc_info{
		.sType = vk::StructureType::eCommandBufferAllocateInfo,
		.commandPool = *m_ve_device.getCommandPool(),
		.level = vk::CommandBufferLevel::ePrimary,
		.commandBufferCount = ve::MAX_FRAMES_IN_FLIGHT
	};
	m_command_buffers = vk::raii::CommandBuffers(m_ve_device.getDevice(), alloc_info);
	assert(m_command_buffers.size() == ve::MAX_FRAMES_IN_FLIGHT && "Failed to allocate command buffers");
	vk::CommandBufferAllocateInfo alloc_info_compute{
		.sType = vk::StructureType::eCommandBufferAllocateInfo,
		.commandPool = *m_ve_device.getComputeCommandPool(),
		.level = vk::CommandBufferLevel::ePrimary,
		.commandBufferCount = ve::MAX_FRAMES_IN_FLIGHT
	};
	m_compute_command_buffers = vk::raii::CommandBuffers(m_ve_device.getDevice(), alloc_info_compute);
	assert(m_compute_command_buffers.size() == ve::MAX_FRAMES_IN_FLIGHT && "Failed to allocate command buffers");
}

void VeRenderer::recreateSwapChain() {
	// Handle minimized window
	auto extent = m_ve_window.getExtent();
	while (extent.width == 0 || extent.height == 0) {
		extent = m_ve_window.getExtent();
		glfwWaitEvents();
	}

	// Create a new swap chain when device is idle
	m_ve_device.getDevice().waitIdle();
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
	VE_LOGI("Swap chain recreated: " << extent.width << "x" << extent.height);
}

// Begin a frame; returns true when a frame is started and command buffer can be used
// Returns false if swap chain is out of date
// throws runtime error if acquire fails for other reasons
bool VeRenderer::beginFrame() {
	assert(!m_is_frame_started && "Can't call beginFrame while already in progress");

	// Wait until image is available
	m_ve_swap_chain->waitForCurrentFence();

	// Acquire an image from the swap chain
	vk::Result result = m_ve_swap_chain->acquireNextImage(&m_current_image_index);
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

	// Retrieve results from the previous time this frame slot was used.
	// Read compute and graphics pairs separately: the compute start timestamp
	// may not be written when particle systems are disabled.
	// Layout per frame: [compute_start, compute_end, graphics_start, graphics_end]
	if (m_query_active[frame_index]) {
		uint32_t base = frame_index * 4;
		float period = m_ve_device.getDeviceProperties().limits.timestampPeriod;
		float ns_to_ms = period / 1000000.0f;

		// Graphics timestamps (always written)
		std::array<uint64_t, 2> gfx_ts;
		vk::Result gfx_result = (*m_ve_device.getDevice()).getQueryPoolResults(
			*m_query_pool, base + 2, 2, gfx_ts.size() * sizeof(uint64_t),
			gfx_ts.data(), sizeof(uint64_t), vk::QueryResultFlagBits::e64
		);
		if (gfx_result == vk::Result::eSuccess) {
			m_gpu_time = static_cast<float>(gfx_ts[1] - gfx_ts[0]) * ns_to_ms;
		}

		// Compute timestamps (start may be missing when particle systems are disabled)
		std::array<uint64_t, 2> comp_ts;
		vk::Result comp_result = (*m_ve_device.getDevice()).getQueryPoolResults(
			*m_query_pool, base, 2, comp_ts.size() * sizeof(uint64_t),
			comp_ts.data(), sizeof(uint64_t), vk::QueryResultFlagBits::e64
		);
		if (comp_result == vk::Result::eSuccess) {
			m_compute_gpu_time = static_cast<float>(comp_ts[1] - comp_ts[0]) * ns_to_ms;

			// Overlap: how much compute and graphics executed simultaneously
			if (gfx_result == vk::Result::eSuccess) {
				uint64_t overlap_start = std::max(comp_ts[0], gfx_ts[0]);
				uint64_t overlap_end = std::min(comp_ts[1], gfx_ts[1]);
				m_gpu_overlap = (overlap_end > overlap_start)
					? static_cast<float>(overlap_end - overlap_start) * ns_to_ms
					: 0.0f;
			}
		} else {
			m_compute_gpu_time = 0.0f;
			m_gpu_overlap = 0.0f;
		}
	}

	command_buffer.reset();
	vk::CommandBufferBeginInfo info{
		.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
	};
	command_buffer.begin(info);

	// Also begin compute command buffer
	auto& compute_command_buffer = getCurrentComputeCommandBuffer();
	compute_command_buffer.reset();
	compute_command_buffer.begin(info);

	// Reset and write start timestamps from each command buffer's own queue
	uint32_t base = frame_index * 4;
	compute_command_buffer.resetQueryPool(*m_query_pool, base, 2);     // compute range [base, base+1]
	// NOTE: compute start timestamp is written by the application before the first compute dispatch.

	command_buffer.resetQueryPool(*m_query_pool, base + 2, 2);         // graphics range [base+2, base+3]
	command_buffer.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *m_query_pool, base + 2);
	m_query_active[frame_index] = true;

	return true;
}

// End the command buffer recording, submits the command buffer and presents the image.
void VeRenderer::endFrame(vk::raii::CommandBuffer& command_buffer) {
	assert(m_is_frame_started && "Can't call endFrame while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't end frame on command buffer from a different frame");

	uint32_t frame_index = m_ve_swap_chain->getCurrentFrame();

	// Write graphics end timestamp
	command_buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *m_query_pool, frame_index * 4 + 3);

	transitionToPresent(command_buffer);
	command_buffer.end();

	// submit graphics and present
	// Submit the command buffer, present the image in accordance with the timeline semaphore values
	auto result = m_ve_swap_chain->submitAndPresent(*command_buffer, &m_current_image_index);

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
}

void VeRenderer::beginDepthPrePass(vk::raii::CommandBuffer& command_buffer) {
	assert(m_is_frame_started && "Can't begin depth pre-pass while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't begin depth pre-pass on command buffer from a different frame");

	auto extent = m_ve_swap_chain->getSwapChainExtent();

	vk::RenderingAttachmentInfo depth_attachment_info = {
		.imageView = *m_ve_swap_chain->getDepthImageView(),
		.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.loadOp = vk::AttachmentLoadOp::eClear,
		.storeOp = vk::AttachmentStoreOp::eStore,
		.clearValue = vk::ClearDepthStencilValue(1.0f, 0)
	};
	vk::RenderingInfo rendering_info = {
		.renderArea = { .offset = { 0, 0 }, .extent = extent },
		.layerCount = 1,
		.colorAttachmentCount = 0,
		.pColorAttachments = nullptr,
		.pDepthAttachment = &depth_attachment_info
	};

	command_buffer.beginRendering(rendering_info);
	command_buffer.setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f));
	command_buffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), extent));
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

// Transitions the swap chain image and multi sampled color image
// to color_attachment_optimal. Begins dynamic rendering.
void VeRenderer::beginSceneRender(vk::raii::CommandBuffer& command_buffer, bool load_depth) {
	assert(m_is_frame_started && "Can't call beginRender while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't begin render on command buffer from a different frame");

	auto extent = m_ve_swap_chain->getSwapChainExtent();
	auto height = extent.height;
	auto width = extent.width;

	// Transition the resolve target to eColorAttachmentOptimal
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
			.resolveMode = vk::ResolveModeFlagBits::eAverage,
			.resolveImageView = *m_ve_swap_chain->getResolveTargetImageView(),
			.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eDontCare,
			.clearValue = vk::ClearColorValue(0.01f, 0.01f, 0.01f, 1.0f)
		};
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
		.clearValue = vk::ClearDepthStencilValue(1.0f, 0)
	};
	vk::RenderingInfo rendering_info = {
		.renderArea = { .offset = { 0, 0 }, .extent = extent },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_attachment_info,
		.pDepthAttachment = &depth_attachment_info
	};

	// Begin dynamic rendering
	command_buffer.beginRendering(rendering_info);
	command_buffer.setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f));
	command_buffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), extent));
}

// Ends the dynamic rendering pass and transitions the swap chain image to presentation
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

void VeRenderer::beginPostProcessRender(vk::raii::CommandBuffer& command_buffer) {
	assert(m_is_frame_started && "Can't call beginPostProcessRender while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't begin post-process on command buffer from a different frame");

	auto extent = m_ve_swap_chain->getSwapChainExtent();

	// Transition the swap chain image to eColorAttachmentOptimal
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
	command_buffer.setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f));
	command_buffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), extent));
}

void VeRenderer::endPostProcessRender(vk::raii::CommandBuffer& command_buffer) {
	assert(m_is_frame_started && "Can't call endPostProcessRender while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't end post-process on command buffer from a different frame");

	command_buffer.endRendering();
}

void VeRenderer::transitionToPresent(vk::raii::CommandBuffer& command_buffer) {
	assert(m_is_frame_started && "Can't call transitionToPresent while frame is not in progress");
	assert(&command_buffer == &getCurrentCommandBuffer() && "Can't transition on command buffer from a different frame");
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

	uint32_t frame_index = m_ve_swap_chain->getCurrentFrame();
	compute_command_buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *m_query_pool, frame_index * 4 + 1);
	compute_command_buffer.end();

	m_ve_swap_chain->submitComputeWork(*compute_command_buffer);
}

}