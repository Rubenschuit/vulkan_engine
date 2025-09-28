#include "ve_app.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>



namespace ve {
	VeApp::VeApp() {
		// First a window, device and swap chain are initialised

		loadModels();
		createDescriptorSetLayout();
		createPipelineLayout();
		createPipeline();
		createCommandBuffers();
		createUniformBuffers();
		createDescriptorPool();
		createDescriptorSets();
	}

	VeApp::~VeApp() {
	}

	void VeApp::run() {
		//std::cout << "max push constants size: " << ve_device.getDeviceProperties().limits.maxPushConstantsSize << " bytes\n";
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

	void VeApp::loadModels() {

		const std::vector<VeModel::Vertex> vertices = {
			{{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
			{{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
			{{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
			{{-0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},

			{{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
			{{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
			{{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
			{{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}}
		};

		const std::vector<uint16_t> indices = {
			0, 1, 2, 2, 3, 0,
			4, 5, 6, 6, 7, 4
		};

		ve_model = std::make_unique<VeModel>(ve_device, vertices, indices);
	}

	void VeApp::createDescriptorSetLayout() {
		vk::DescriptorSetLayoutBinding binding{
			.binding = 0,
			.descriptorType = vk::DescriptorType::eUniformBuffer,
			.descriptorCount = 1,
			.stageFlags = vk::ShaderStageFlagBits::eVertex,
			.pImmutableSamplers = nullptr
		};
		vk::DescriptorSetLayoutCreateInfo layout_info{
			.flags = {},
			.bindingCount = 1,
			.pBindings = &binding
		};
		descriptor_set_layout = vk::raii::DescriptorSetLayout(ve_device.getDevice(), layout_info);
	}

	void VeApp::createPipelineLayout() {
		vk::PipelineLayoutCreateInfo pipeline_layout_info{
			.sType = vk::StructureType::ePipelineLayoutCreateInfo,
			.setLayoutCount = 1,
			.pSetLayouts = &*descriptor_set_layout,
			.pushConstantRangeCount = 0,
			.pPushConstantRanges = nullptr
		};
		pipeline_layout = vk::raii::PipelineLayout(ve_device.getDevice(), pipeline_layout_info);
	}

	void VeApp::createPipeline() {
		PipelineConfigInfo pipeline_config{};
		VePipeline::defaultPipelineConfigInfo(pipeline_config);
		// set formats for dynamic rendering
		pipeline_config.color_format = ve_swap_chain->getSwapChainImageFormat();
		assert(pipeline_layout != nullptr && "Pipeline layout is null");
		pipeline_config.pipeline_layout = pipeline_layout;
		ve_pipeline = std::make_unique<VePipeline>(
			ve_device,
			"../shaders/simple_shader.spv",
			pipeline_config);
		assert(ve_pipeline != nullptr && "Failed to create pipeline");

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
		assert(command_buffers.size() == ve::MAX_FRAMES_IN_FLIGHT && "Failed to allocate command buffers");
	}

	// Record commands into the command buffer for the given image index.
	// Uses barriers to transition image layouts before and after rendering.
	void VeApp::recordCommandBuffer(uint32_t image_index) {
		auto extent = ve_swap_chain->getSwapChainExtent();
		auto height = extent.height;
		auto width = extent.width;
		uint32_t current_frame = ve_swap_chain->getCurrentFrame();
		assert(current_frame < command_buffers.size() && "Current frame index out of bounds");
		assert(image_index < ve_swap_chain->getImageCount() && "Image index out of bounds");

		command_buffers[current_frame].begin({});
		// Transition the swap chain image to eColorAttachmentOptimal
		transitionImageLayout(
			image_index,
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eColorAttachmentOptimal,
			{},
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput
		);

		// Transition depth image layout, TODO add function for this
        vk::ImageMemoryBarrier2 depth_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = {},
            .dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
            .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = ve_swap_chain->getDepthImage(),
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eDepth,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        vk::DependencyInfo depth_dependency_info = {
            .dependencyFlags = {},
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &depth_barrier
        };
        command_buffers[current_frame].pipelineBarrier2(depth_dependency_info);

		// Setup dynamic rendering info
		vk::RenderingAttachmentInfo attachment_info = {
			.imageView = ve_swap_chain->getSwapChainImageViews()[image_index],
			.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearColorValue(0.01f, 0.01f, 0.01f, 1.0f)
		};
		vk::RenderingAttachmentInfo depth_attachment_info = {
			.imageView = ve_swap_chain->getDepthImageView(),
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
		command_buffers[current_frame].beginRendering(rendering_info);
		command_buffers[current_frame].bindPipeline(vk::PipelineBindPoint::eGraphics, ve_pipeline->getPipeline());
		command_buffers[current_frame].setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f));
		command_buffers[current_frame].setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), extent));
		ve_model->bindVertexBuffer(command_buffers[current_frame]);
		ve_model->bindIndexBuffer(command_buffers[current_frame]);
		command_buffers[current_frame].bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics,
			*pipeline_layout,
			0,
			*descriptor_sets[current_frame],
			{}
		);
		ve_model->drawIndexed(command_buffers[current_frame]);
		command_buffers[current_frame].endRendering();

		// After rendering, transition swap chain image to presentation
		transitionImageLayout(
			image_index,
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::ImageLayout::ePresentSrcKHR,
			vk::AccessFlagBits2::eColorAttachmentWrite,
			{},
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eBottomOfPipe
		);
		command_buffers[current_frame].end();
	}

	// Transition the image layout of the given swap chain image using
	// pipeline barriers to ensure proper synchronization
	void VeApp::transitionImageLayout(
				uint32_t image_index,
				vk::ImageLayout old_layout,
				vk::ImageLayout new_layout,
				vk::AccessFlags2 src_access_mask,
				vk::AccessFlags2 dst_access_mask,
				vk::PipelineStageFlags2 src_stage_mask,
				vk::PipelineStageFlags2 dst_stage_mask) {

		assert(image_index < ve_swap_chain->getImageCount() && "Image index out of bounds");
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
		auto extent = ve_window.getExtent();
		while (extent.width == 0 || extent.height == 0) {
			extent = ve_window.getExtent();
			glfwWaitEvents();
		}

		// Create a new swap chain when device is idle
		ve_device.getDevice().waitIdle();
		extent = ve_window.getExtent();
		if (ve_swap_chain == nullptr) {
			ve_swap_chain = std::make_unique<VeSwapChain>(ve_device, extent);
		} else {
			// Transfer ownership of the existing swap chain to a shared_ptr so the new one
			// can safely reference it during recreation.
			std::shared_ptr<VeSwapChain> old_swap_chain{ std::move(ve_swap_chain) };
			ve_swap_chain = std::make_unique<VeSwapChain>(ve_device, extent, old_swap_chain);
			if (!old_swap_chain->compareSwapFormats(*ve_swap_chain)) {
				throw std::runtime_error("Swap chain image (or depth) format has changed!");
			}
		}
	}

	void VeApp::createUniformBuffers() {
		vk::DeviceSize buffer_size = sizeof(UniformBufferObject);
		assert(buffer_size > 0 && "Uniform buffer size is zero");
		assert(buffer_size % 16 == 0 && "Uniform buffer size must be a multiple of 16 bytes");
		assert(buffer_size <= ve_device.getDeviceProperties().limits.maxUniformBufferRange && "Uniform buffer size exceeds maximum limit");

		uniform_buffers.clear();

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
			uniform_buffers.emplace_back(std::make_unique<VeBuffer>(
				ve_device,
				buffer_size,
				1,
				vk::BufferUsageFlagBits::eUniformBuffer,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
				ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
			));
			uniform_buffers[i]->map();
		}
	}

	void VeApp::updateUniformBuffer(uint32_t current_image) {
		// this assertion saved me a lot of debugging time
		assert(current_image < uniform_buffers.size() && "Current image index out of bounds");

		// Calculate elapsed time since start of the program
		static auto start_time = std::chrono::high_resolution_clock::now();
		auto current_time = std::chrono::high_resolution_clock::now();
    	float time = std::chrono::duration<float, std::chrono::seconds::period>(current_time - start_time).count();

		UniformBufferObject ubo{};
		// Coordinate system explanation:
		//  Camera is at (2,2,2), looking at the origin (0,0,0)
		//
		//		+Y (up)
		//		^
		//		|
		//		|
		//		O------> +X (right)
		//	   /
		//	  /
		//	+Z

		// Rotate the model, centered at the origin, over time, around the Z axis
		ubo.model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		ubo.proj = glm::perspective(glm::radians(45.0f), ve_swap_chain->extentAspectRatio(), 0.1f, 10.0f);
		// GLM was originally designed for OpenGL, where the Y coordinate of the clip coordinates is inverted.
		ubo.proj[1][1] *= -1;

		uniform_buffers[current_image]->writeToBuffer(&ubo);
	}

	void VeApp::createDescriptorPool() {
		vk::DescriptorPoolSize pool_size{
			.type = vk::DescriptorType::eUniformBuffer,
			.descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)
		};
		vk::DescriptorPoolCreateInfo pool_info{
			.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
			.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
			.poolSizeCount = 1,
			.pPoolSizes = &pool_size
		};
		descriptor_pool = vk::raii::DescriptorPool(ve_device.getDevice(), pool_info);
	}

	void VeApp::createDescriptorSets() {
		std::vector<vk::DescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, *descriptor_set_layout);
		vk::DescriptorSetAllocateInfo alloc_info{
			.descriptorPool = *descriptor_pool,
			.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
			.pSetLayouts = layouts.data()
		};
		descriptor_sets.clear();
		descriptor_sets = vk::raii::DescriptorSets(ve_device.getDevice(), alloc_info);

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
			vk::DescriptorBufferInfo buffer_info{
				.buffer = *uniform_buffers[i]->getBuffer(),
				.offset = 0,
				.range = sizeof(UniformBufferObject)
			};
			vk::WriteDescriptorSet descriptor_write{
				.dstSet = *descriptor_sets[i],
				.dstBinding = 0,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = vk::DescriptorType::eUniformBuffer,
				.pImageInfo = nullptr,
				.pBufferInfo = &buffer_info,
				.pTexelBufferView = nullptr
			};
			ve_device.getDevice().updateDescriptorSets(descriptor_write, {});
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

		// Update
		uint32_t current_frame = ve_swap_chain->getCurrentFrame();
		updateUniformBuffer(current_frame);

		// Reset the fence for the current frame
		ve_swap_chain->resetFences();

		// Record command buffer using the acquired image
		assert(current_frame < command_buffers.size() && "Current frame index out of bounds");
		command_buffers[current_frame].reset();
		recordCommandBuffer(image_index);

		// Submit the command buffer, present the image
		result = ve_swap_chain->submitAndPresent(*command_buffers[current_frame], &image_index);
		if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR || ve_window.wasWindowResized()) {
			ve_window.resetWindowResizedFlag();
			recreateSwapChain();
		} else if (result != vk::Result::eSuccess) {
			throw std::runtime_error("failed to present swap chain image!");
		}
		// Advance to the next frame
		ve_swap_chain->advanceFrame();

		// Update FPS counter and window title roughly once per second
		updateFpsWindowTitle();
	}

	void VeApp::updateFpsWindowTitle() {
		using clock = std::chrono::high_resolution_clock;
		fps_frame_count++;
		auto now = clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_last_time);
		if (elapsed.count() >= 1000) {
			// Compute FPS and update title
			double fps = static_cast<double>(fps_frame_count) * 1000.0 / static_cast<double>(elapsed.count());
			std::string title = std::string("Vulkan Engine!  ") + std::to_string(static_cast<int>(fps)) + " FPS";
			glfwSetWindowTitle(ve_window.getGLFWwindow(), title.c_str());
			// Reset
			fps_frame_count = 0;
			fps_last_time = now;
		}
	}
}

