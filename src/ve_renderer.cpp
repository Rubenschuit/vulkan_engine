#include "pch.hpp"
#include "ve_renderer.hpp"

#include <stdexcept>


namespace ve {
	VeRenderer::VeRenderer(VeDevice& device, VeWindow& window) : m_ve_device(device), m_ve_window(window) {
		m_ve_swap_chain = std::make_unique<VeSwapChain>(m_ve_device, m_ve_window.getExtent());
		createCommandBuffers();
	}

	VeRenderer::~VeRenderer() {}

	void VeRenderer::createCommandBuffers() {
		vk::CommandBufferAllocateInfo alloc_info{
			.sType = vk::StructureType::eCommandBufferAllocateInfo,
			.commandPool = *m_ve_device.getCommandPool(),
			.level = vk::CommandBufferLevel::ePrimary,
			.commandBufferCount = ve::MAX_FRAMES_IN_FLIGHT
		};
		m_command_buffers = vk::raii::CommandBuffers(m_ve_device.getDevice(), alloc_info);
		assert(m_command_buffers.size() == ve::MAX_FRAMES_IN_FLIGHT && "Failed to allocate command buffers");
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
			m_ve_swap_chain = std::make_unique<VeSwapChain>(m_ve_device, extent);
		} else {
			// Transfer ownership of the existing swap chain to a shared_ptr so the new one
			// can safely reference it during recreation.
			std::shared_ptr<VeSwapChain> old_swap_chain{ std::move(m_ve_swap_chain) };
			m_ve_swap_chain = std::make_unique<VeSwapChain>(m_ve_device, extent, old_swap_chain);
			if (!old_swap_chain->compareSwapFormats(*m_ve_swap_chain)) {
				throw std::runtime_error("Swap chain image (or depth) format has changed!");
				// Todo: Handle swap chain format changes (e.g. recreate pipelines)
			}
		}
		VE_LOGI("Swap chain recreated: " << extent.width << "x" << extent.height);
	}

	// Begin a frame; returns true when a frame is started and command buffer can be used
	bool VeRenderer::beginFrame() {
		assert(!m_is_frame_started && "Can't call beginFrame while already in progress");

		// Wait until the previous frame is finished
		m_ve_swap_chain->waitForFences();

		// Acquire an image from the swap chain
		auto result = m_ve_swap_chain->acquireNextImage(&m_current_image_index);
		if (result == vk::Result::eErrorOutOfDateKHR) {
			recreateSwapChain();
			return false;
		}
		if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
			throw std::runtime_error("failed to acquire swap chain image!");
		}

		m_is_frame_started = true;

		// Reset the fence for the current frame
		m_ve_swap_chain->resetFences();

		// Record command buffer using the acquired image
		auto& command_buffer = getCurrentCommandBuffer();

		command_buffer.reset();
		command_buffer.begin({});
		return true;
	}

	void VeRenderer::endFrame(vk::raii::CommandBuffer& command_buffer) {
		assert(m_is_frame_started && "Can't call endFrame while frame is not in progress");
		assert(&command_buffer == &getCurrentCommandBuffer() && "Can't end frame on command buffer from a different frame");
		command_buffer.end();

		// Submit the command buffer, present the image
		auto result = m_ve_swap_chain->submitAndPresent(command_buffer, &m_current_image_index);
		if (result == vk::Result::eErrorOutOfDateKHR ||
				result == vk::Result::eSuboptimalKHR ||
				m_ve_window.wasWindowResized()) {
			m_ve_window.resetWindowResizedFlag();
			recreateSwapChain();
		} else if (result != vk::Result::eSuccess) {
			throw std::runtime_error("failed to present swap chain image!");
		}
		// Advance to the next frame
		m_ve_swap_chain->advanceFrame();
		m_is_frame_started = false;
	}

	void VeRenderer::beginRender(vk::raii::CommandBuffer& command_buffer) {
		assert(m_is_frame_started && "Can't call beginRender while frame is not in progress");
		assert(&command_buffer == &getCurrentCommandBuffer() && "Can't begin render on command buffer from a different frame");

		auto extent = m_ve_swap_chain->getSwapChainExtent();
		auto height = extent.height;
		auto width = extent.width;

		// Transition the swap chain image to eColorAttachmentOptimal
		m_ve_swap_chain->transitionImageLayout(
			command_buffer,
			m_current_image_index,
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eColorAttachmentOptimal,
			{},
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput
		);

		// Setup dynamic rendering attachments
		vk::RenderingAttachmentInfo attachment_info = {
			.imageView = m_ve_swap_chain->getSwapChainImageViews()[m_current_image_index],
			.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearColorValue(0.01f, 0.01f, 0.01f, 1.0f)
		};
		vk::RenderingAttachmentInfo depth_attachment_info = {
			.imageView = m_ve_swap_chain->getDepthImageView(),
			.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eDontCare,
			.clearValue = vk::ClearDepthStencilValue(1.0f, 0)
		};
		vk::RenderingInfo rendering_info = {
			.renderArea = { .offset = { 0, 0 }, .extent = extent },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &attachment_info,
			.pDepthAttachment = &depth_attachment_info
		};

		// Begin dynamic rendering
		command_buffer.beginRendering(rendering_info);
		command_buffer.setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f));
		command_buffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), extent));
	}

	void VeRenderer::endRender(vk::raii::CommandBuffer& command_buffer) {
		assert(m_is_frame_started && "Can't call endRender while frame is not in progress");
		assert(&command_buffer == &getCurrentCommandBuffer() && "Can't end render on command buffer from a different frame");

		command_buffer.endRendering();

		// After rendering, transition swap chain image to presentation
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

}