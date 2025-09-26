#include "ve_app.hpp"

namespace ve {
	VeApp::VeApp() {
		loadModels();
		createPipelineLayout();
		createPipeline();
		createCommandBuffers();
	}

	VeApp::~VeApp() {
	}

	void VeApp::run() {
		mainLoop();
		cleanup();
	}

	void VeApp::mainLoop() {
		while (!glfwWindowShouldClose(ve_window.getGLFWwindow())) {
			glfwPollEvents();
			drawFrame();
		}
		ve_device.getDevice().waitIdle();
	}

	void VeApp::cleanup() {
		// nothing yet
	}

	// Recursive function to generate Sierpinski triangle vertices
	void recursionTriangles(glm::vec2 A, glm::vec2 B, glm::vec2 C, int depth, int max_depth, std::vector<VeModel::Vertex> &vertices) {
		if (depth > max_depth)
			return;
		glm::vec2 D = (A + B) / 2.0f;
		glm::vec2 E = (A + C) / 2.0f;
		glm::vec2 F = (B + C) / 2.0f;
		glm::vec3 col_D{1.0f, 0.0f, 0.0f};
		glm::vec3 col_E{0.0f, 1.0f, 0.0f};
		glm::vec3 col_F{0.0f, 0.0f, 1.0f};

		if (depth % 1 == 0) {
			col_D = { 0.0f, 0.0f, 0.0f };
			col_E = { 0.0f, 0.0f, 0.0f };
			col_F = { 0.0f, 0.0f, 0.0f };
		}

		vertices.emplace_back(D, col_D);
		vertices.emplace_back(E, col_E);
		vertices.emplace_back(F, col_F);

		recursionTriangles(A, E, D, depth + 1, max_depth, vertices);
		recursionTriangles(D, B, F, depth + 1, max_depth, vertices);
		recursionTriangles(F, E, C, depth + 1, max_depth, vertices);
		//recursionTriangles(D, E, F, depth + 1, max_depth, vertices);
	};

	void VeApp::loadModels() {
		std::vector<VeModel::Vertex> vertices = {
			{{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
			{{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},
			{{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}
		};


		int max_depth = 6; // Adjust for desired recursion depth
		recursionTriangles(vertices[0].pos, vertices[1].pos, vertices[2].pos, 0, max_depth, vertices);
		ve_model = std::make_unique<VeModel>(ve_device, vertices);
	}

	void VeApp::createPipelineLayout() {
		vk::PipelineLayoutCreateInfo pipeline_layout_info{
			.sType = vk::StructureType::ePipelineLayoutCreateInfo,
			.setLayoutCount = 0,
			.pSetLayouts = nullptr,
			.pushConstantRangeCount = 0,
			.pPushConstantRanges = nullptr
		};
		pipeline_layout = vk::raii::PipelineLayout(ve_device.getDevice(), pipeline_layout_info);
	}

	void VeApp::createPipeline() {
		auto pipeline_config = VePipeline::defaultPipelineConfigInfo();
		// set formats for dynamic rendering
		pipeline_config.color_format = ve_swap_chain->getSwapChainImageFormat();
		pipeline_config.pipeline_layout = pipeline_layout;
		ve_pipeline = std::make_unique<VePipeline>(
			ve_device,
			"../shaders/simple_shader.spv",
			pipeline_config);
	}

	void VeApp::createCommandBuffers() {
		// Allocate a single command buffer for now
		vk::CommandBufferAllocateInfo alloc_info{
			.sType = vk::StructureType::eCommandBufferAllocateInfo,
			.commandPool = *ve_device.getCommandPool(),
			.level = vk::CommandBufferLevel::ePrimary,
			.commandBufferCount = ve::MAX_FRAMES_IN_FLIGHT
		};
		command_buffers = vk::raii::CommandBuffers(ve_device.getDevice(), alloc_info);
	}

	void VeApp::recordCommandBuffer(uint32_t imageIndex) {
		auto extent = ve_swap_chain->getSwapChainExtent();
		auto height = extent.height;
		auto width = extent.width;
		uint32_t current_frame = ve_swap_chain->getCurrentFrame();

		command_buffers[current_frame].begin({});
		// Before starting rendering, transition the swapchain image to COLOR_ATTACHMENT_OPTIMAL
		transitionImageLayout(
			imageIndex,
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eColorAttachmentOptimal,
			{},
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput
		);
		vk::ClearValue clearColor = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f);
		vk::RenderingAttachmentInfo attachmentInfo = {
			.imageView = ve_swap_chain->getSwapChainImageViews()[imageIndex],
			.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = clearColor
		};
		vk::RenderingInfo renderingInfo = {
			.renderArea = { .offset = { 0, 0 }, .extent = extent },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &attachmentInfo
		};

		command_buffers[current_frame].beginRendering(renderingInfo);
		command_buffers[current_frame].bindPipeline(vk::PipelineBindPoint::eGraphics, ve_pipeline->getPipeline());
		command_buffers[current_frame].setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f));
		command_buffers[current_frame].setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), extent));
		ve_model->bind(command_buffers[current_frame]);
		ve_model->draw(command_buffers[current_frame]);
		command_buffers[current_frame].endRendering();
		// After rendering, transition to presentation
		transitionImageLayout(
			imageIndex,
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::ImageLayout::ePresentSrcKHR,
			vk::AccessFlagBits2::eColorAttachmentWrite,
			{},
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eBottomOfPipe
		);
		command_buffers[current_frame].end();
	}

	void VeApp::transitionImageLayout(
		uint32_t image_index,
		vk::ImageLayout old_layout,
		vk::ImageLayout new_layout,
		vk::AccessFlags2 src_access_mask,
		vk::AccessFlags2 dst_access_mask,
		vk::PipelineStageFlags2 src_stage_mask,
		vk::PipelineStageFlags2 dst_stage_mask
	) {
		vk::ImageMemoryBarrier2 barrier = {
			.srcStageMask = src_stage_mask,
			.srcAccessMask = src_access_mask,
			.dstStageMask = dst_stage_mask,
			.dstAccessMask = dst_access_mask,
			.oldLayout = old_layout,
			.newLayout = new_layout,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = ve_swap_chain->getSwapChainImages()[image_index],
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
		uint32_t current_frame = ve_swap_chain->getCurrentFrame();
		command_buffers[current_frame].pipelineBarrier2(dependency_info);
	}

	void VeApp::recreateSwapChain() {
		// Handle minimized window
		int width = 0, height = 0;
		glfwGetFramebufferSize(ve_window.getGLFWwindow(), &width, &height);
		while (width == 0 || height == 0) {
			glfwGetFramebufferSize(ve_window.getGLFWwindow(), &width, &height);
			glfwWaitEvents();
		}

		ve_device.getDevice().waitIdle();

		// Create a new swap chain
		if (ve_swap_chain == nullptr) {
			ve_swap_chain = std::make_unique<VeSwapChain>(ve_device, ve_window.getExtent());
		} else {
			// Transfer ownership of the existing swap chain to a shared_ptr so the new one
			// can safely reference it during recreation.
			std::shared_ptr<VeSwapChain> old_swap_chain{ std::move(ve_swap_chain) };
			ve_swap_chain = std::make_unique<VeSwapChain>(ve_device, ve_window.getExtent(), old_swap_chain);
			if (!old_swap_chain->compareSwapFormats(*ve_swap_chain)) {
				throw std::runtime_error("Swap chain image (or depth) format has changed!");
			}
		}
	}

	void VeApp::drawFrame() {
		// Wait until the previous frame is finished
		ve_swap_chain->waitForFences();

		// Acquire an image from the swap chain
		uint32_t image_index;
		auto result = ve_swap_chain->acquireNextImage(&image_index);
		if (result == vk::Result::eErrorOutOfDateKHR) {
			recreateSwapChain();
			return;
		}
		if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
			throw std::runtime_error("failed to acquire swap chain image!");
		}

		// Reset the fence for the current frame
		ve_swap_chain->resetFences();

		// Record command buffer using the acquired image
		uint32_t current_frame = ve_swap_chain->getCurrentFrame();
		command_buffers[current_frame].reset();
		recordCommandBuffer(image_index);

		// Submit the command buffer
		result = ve_swap_chain->submitAndPresent(*command_buffers[current_frame], &image_index);
		if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR || ve_window.framebuffer_resized) {
			ve_window.framebuffer_resized = false;
			recreateSwapChain();
		} else if (result != vk::Result::eSuccess) {
			throw std::runtime_error("failed to present swap chain image!");
		}
		// Advance to the next frame
		ve_swap_chain->advanceFrame();
	}
}

