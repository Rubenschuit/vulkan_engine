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
		createUniformBuffers();
		createDescriptorPool();
		createDescriptorSets();
	}

	VeApp::~VeApp() {
	}

	void VeApp::run() {
		//std::cout << "max push constants size: " << ve_device.getDeviceProperties().limits.maxPushConstantsSize << " bytes\n";
		mainLoop();
	}

	void VeApp::mainLoop() {
		while (!glfwWindowShouldClose(ve_window.getGLFWwindow())) {
			glfwPollEvents();
			auto [result, command_buffer] = ve_renderer.beginFrame();
			if (result) {
				// update
				auto current_frame = ve_renderer.getCurrentFrame();
				updateUniformBuffer(current_frame);
				updateFpsWindowTitle();

				//rendering
				ve_renderer.beginRender(command_buffer);
				drawFrame(command_buffer, current_frame);
				ve_renderer.endRender(command_buffer);
				ve_renderer.endFrame(command_buffer);
			}
		}
		// Ensure device is idle before destroying resources
		ve_device.getDevice().waitIdle();
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
		pipeline_config.color_format = ve_renderer.getSwapChainImageFormat();
		assert(pipeline_layout != nullptr && "Pipeline layout is null");
		pipeline_config.pipeline_layout = pipeline_layout;
		ve_pipeline = std::make_unique<VePipeline>(
			ve_device,
			"../shaders/simple_shader.spv",
			pipeline_config);
		assert(ve_pipeline != nullptr && "Failed to create pipeline");

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

	void VeApp::updateUniformBuffer(uint32_t current_frame) {
		// this assertion saved me a lot of debugging time
		assert(current_frame < uniform_buffers.size() && "Current image index out of bounds");

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
		ubo.proj = glm::perspective(glm::radians(45.0f), ve_renderer.getExtentAspectRatio(), 0.1f, 10.0f);
		// GLM was originally designed for OpenGL, where the Y coordinate of the clip coordinates is inverted.
		ubo.proj[1][1] *= -1;

		uniform_buffers[current_frame]->writeToBuffer(&ubo);
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

	void VeApp::drawFrame(vk::raii::CommandBuffer& command_buffer, uint32_t current_frame) {
		command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, ve_pipeline->getPipeline());
		ve_model->bindVertexBuffer(command_buffer);
		ve_model->bindIndexBuffer(command_buffer);
		command_buffer.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics,
			*pipeline_layout,
			0,
			*descriptor_sets[current_frame],
			{}
		);
		ve_model->drawIndexed(command_buffer);
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

