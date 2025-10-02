#include "pch.hpp"
#include "ve_app.hpp"
#include "simple_render_system.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>

namespace ve {
	VeApp::VeApp() {
		// First a window, device and swap chain are initialised

		loadModels();
		createDescriptorSetLayout();
		createUniformBuffers();
		createDescriptorPool();
		createDescriptorSets();
	}

	VeApp::~VeApp() {}

	void VeApp::run() {
		//std::cout << "max push constants size: " << ve_device.getDeviceProperties().limits.maxPushConstantsSize << " bytes\n";
		mainLoop();
	}

	void VeApp::mainLoop() {
		SimpleRenderSystem simple_render_system(ve_device, descriptor_set_layout, ve_renderer.getSwapChainImageFormat());

		while (!glfwWindowShouldClose(ve_window.getGLFWwindow())) {
			glfwPollEvents();
			if (ve_renderer.beginFrame()) {  // Next image acquired successfully

				// Setup frame info
				auto& command_buffer = ve_renderer.getCurrentCommandBuffer();
				auto current_frame = ve_renderer.getCurrentFrame();
				VeFrameInfo frame_info{
					.global_descriptor_set = descriptor_sets[current_frame],
					.command_buffer = command_buffer,
					.ve_model = *ve_model,
				};

				// update
				updateUniformBuffer(current_frame);
				updateFpsWindowTitle();

				//rendering
				ve_renderer.beginRender(command_buffer);

				simple_render_system.drawFrame(frame_info);

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
		};

		const std::vector<uint16_t> indices = {
			0, 1, 2, 2, 3, 0
		};

		ve_model = std::make_unique<VeModel>(ve_device, vertices, indices);
	}

	// One binding for the uniform buffer, another for the texture sampler
	void VeApp::createDescriptorSetLayout() {
		std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {
			vk::DescriptorSetLayoutBinding{
				.binding = 0,
				.descriptorType = vk::DescriptorType::eUniformBuffer,
				.descriptorCount = 1,
				.stageFlags = vk::ShaderStageFlagBits::eVertex,
				.pImmutableSamplers = nullptr
			},
			vk::DescriptorSetLayoutBinding{
				.binding = 1,
				.descriptorType = vk::DescriptorType::eCombinedImageSampler,
				.descriptorCount = 1,
				.stageFlags = vk::ShaderStageFlagBits::eFragment,
				.pImmutableSamplers = nullptr
			}
		};

		vk::DescriptorSetLayoutCreateInfo layout_info{
			.flags = {},
			.bindingCount = bindings.size(),
			.pBindings = bindings.data()
		};
		descriptor_set_layout = vk::raii::DescriptorSetLayout(ve_device.getDevice(), layout_info);
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

	void VeApp::createDescriptorPool() {
		std::array<vk::DescriptorPoolSize, 2> pool_sizes {
			vk::DescriptorPoolSize {
				.type = vk::DescriptorType::eUniformBuffer,
				.descriptorCount = MAX_FRAMES_IN_FLIGHT
			},
			vk::DescriptorPoolSize {
				.type = vk::DescriptorType::eCombinedImageSampler,
				.descriptorCount = MAX_FRAMES_IN_FLIGHT
			}
		};
		vk::DescriptorPoolCreateInfo pool_info{
			.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
			.maxSets = MAX_FRAMES_IN_FLIGHT,
			.poolSizeCount = pool_sizes.size(),
			.pPoolSizes = pool_sizes.data()
		};

		descriptor_pool = vk::raii::DescriptorPool(ve_device.getDevice(), pool_info);
	}

	// Create a descriptor set for each frame in flight and then write the buffer and image info to it
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
			vk::DescriptorImageInfo image_info{
				.sampler = texture.getSampler(),
				.imageView = texture.getImageView(),
				.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
			};
			std::array<vk::WriteDescriptorSet, 2> descriptor_writes = {
				vk::WriteDescriptorSet {
					.dstSet = *descriptor_sets[i],
					.dstBinding = 0,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = vk::DescriptorType::eUniformBuffer,
					.pImageInfo = nullptr,
					.pBufferInfo = &buffer_info,
					.pTexelBufferView = nullptr
				},
				vk::WriteDescriptorSet {
					.dstSet = *descriptor_sets[i],
					.dstBinding = 1,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = vk::DescriptorType::eCombinedImageSampler,
					.pImageInfo = &image_info,
					.pBufferInfo = nullptr,
					.pTexelBufferView = nullptr
				}
			};
			// Update the descriptor sets with the buffer and image info
			ve_device.getDevice().updateDescriptorSets(descriptor_writes, {});
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
		// GLM was originally designed for OpenGL, where the Y coordinate of the clip coordinates is inverted
		ubo.proj[1][1] *= -1;
		float x = time;
		ubo.offset = glm::vec2(2.0f * glm::sin(x), 2.0f * glm::cos(x ) - 1.0f);

		uniform_buffers[current_frame]->writeToBuffer(&ubo);
	}

	void VeApp::updateFpsWindowTitle() {
		// Per-frame delta using steady clock
		auto now = clock::now();
		last_frame_ms = std::chrono::duration<double, std::milli>(now - last_frame_time).count();
		last_frame_time = now;

		// Accumulate into a 1-second window
		sum_frame_ms += last_frame_ms;
		fps_frame_count++;
		auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_window_start).count();
		if (window_ms >= 1000) {
			double fps = (window_ms > 0) ? (1000.0 * static_cast<double>(fps_frame_count) / static_cast<double>(window_ms)) : 0.0;
			double avg_ms = (fps_frame_count > 0) ? (sum_frame_ms / static_cast<double>(fps_frame_count)) : 0.0;
			char buf[128];
			snprintf(buf, sizeof(buf), "Vulkan Engine!  %d FPS  %.2f ms", static_cast<int>(fps), avg_ms);
			glfwSetWindowTitle(ve_window.getGLFWwindow(), buf);
			// Reset window counters
			fps_frame_count = 0;
			sum_frame_ms = 0.0;
			fps_window_start = now;
		}
	}
}

