#include "pch.hpp"
#include "ve_app.hpp"
#include "systems/simple_render_system.hpp"
#include "systems/axes_render_system.hpp"
#include "systems/point_light_system.hpp"


#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>

namespace ve {
	VeApp::VeApp() {
		// First a window, device and swap chain are initialised
		loadGameObjects();
		createUniformBuffers();
		createDescriptors();

		// Initialise camera
		last_aspect = ve_renderer.getExtentAspectRatio();
		camera.setPerspective(fov, last_aspect, near_plane, far_plane);
	}

	VeApp::~VeApp() {}

	void VeApp::run() {
		SimpleRenderSystem simple_render_system(
			ve_device,
			global_set_layout->getDescriptorSetLayout(),
			material_set_layout->getDescriptorSetLayout(),
			ve_renderer.getSwapChainImageFormat()
		);
		AxesRenderSystem axes_render_system(
			ve_device,
			global_set_layout->getDescriptorSetLayout(),
			ve_renderer.getSwapChainImageFormat()
		);
		PointLightSystem point_light_system(
			ve_device,
			global_set_layout->getDescriptorSetLayout(),
			material_set_layout->getDescriptorSetLayout(),
			ve_renderer.getSwapChainImageFormat()
		);
		auto current_time = std::chrono::high_resolution_clock::now();

		while (!glfwWindowShouldClose(ve_window.getGLFWwindow())) {
			glfwPollEvents();
			if (ve_renderer.beginFrame()) {  // Next image acquired successfully
				// Calculate frame time
				auto new_time = std::chrono::high_resolution_clock::now();
				frame_time = std::chrono::duration<float, std::chrono::seconds::period>(new_time - current_time).count();
				current_time = new_time;

				// Setup frame info
				auto& command_buffer = ve_renderer.getCurrentCommandBuffer();
				auto current_frame = ve_renderer.getCurrentFrame();
				VeFrameInfo frame_info{
					.global_descriptor_set = global_descriptor_sets[current_frame],
					.material_descriptor_set = material_descriptor_set,
					.command_buffer = command_buffer,
					.game_objects = game_objects,
					.frame_time = frame_time
				};
				// update
				input_controller.processInput(frame_time, camera);
				updateCamera();
				updateWindowTitle();

				UniformBufferObject ubo{};
				point_light_system.update(frame_info, ubo);
				updateUniformBuffer(current_frame, ubo);

				//rendering
				ve_renderer.beginRender(command_buffer);

				simple_render_system.renderObjects(frame_info);
				axes_render_system.renderAxes(frame_info);
				point_light_system.render(frame_info);

				ve_renderer.endRender(command_buffer);
				ve_renderer.endFrame(command_buffer);
			}
		}
		// Ensure device is idle before destroying resources
		ve_device.getDevice().waitIdle();
	}

	void VeApp::loadGameObjects() {
		// Create 10 point lights with ranging over all colors
		const uint num_lights = 10; // max 10
		const glm::vec3 colors[10] = {
			{1.0f, 1.0f, 1.0f}, //white
			{1.0f, 0.0f, 0.0f}, //red
			{1.0f, 0.5f, 0.0f}, //orange
			{1.0f, 1.0f, 0.0f}, //yellow
			{0.0f, 1.0f, 0.0f}, //green
			{0.0f, 1.0f, 0.5f}, //turquoise
			{0.0f, 1.0f, 1.0f}, //cyan
			{0.0f, 0.5f, 1.0f}, //light-blue
			{0.0f, 0.0f, 1.0f}, //blue
			{0.5f, 0.0f, 1.0f}  //purple
		};
		constexpr float radius = 25.0f;
		constexpr float height = 10.0f;

		for (uint i = 0; i < num_lights; i += 2) {
			auto point_light = VeGameObject::createPointLight(1.1f, 10.0f, colors[i % 10]);
			glm::vec3 pos = {radius * cos(glm::two_pi<float>() / num_lights * i), radius * sin(glm::two_pi<float>() / num_lights * i), height};
			point_light.transform.translation = pos;
			game_objects.emplace(point_light.getId(), std::move(point_light));
		}

		// Floor
		VeGameObject floor = VeGameObject::createGameObject();
		auto quad = std::make_shared<VeModel>(ve_device, "../models/quad.obj");
		floor.ve_model = quad;
		floor.has_texture = 0.0f;
		floor.transform = {
			.translation = {0.0f, 0.0f, -0.1f},
			.rotation = {glm::radians(-90.0f), 0.0f, 0.0f},
			.scale = {40.0f, 40.0f, 40.0f}
		};
		game_objects.emplace(floor.getId(), std::move(floor));

		std::shared_ptr<VeModel> model = std::make_shared<VeModel>(ve_device, "../models/viking_room.obj");
		for (int j = 0; j < 10; j++) {
			for (int i = 0; i < 10; i++) {
				VeGameObject obj = VeGameObject::createGameObject();
				obj.ve_model = model;
				obj.transform.translation = {i * 4.0f, j * 4.0f, 0.f};
				obj.has_texture = 1.0f;
				//obj.scale = {0.4f + 0.2f * j, 0.4f + 0.2f * j, 1.0f};
				//obj.color = {1.0f, 1.0f, 1.0f};
				game_objects.emplace(obj.getId(), std::move(obj));
			}
		}
		std::shared_ptr<VeModel> model2 = std::make_shared<VeModel>(ve_device, "../models/cube.obj");
		for (int j = 0; j < 10; j++) {
			for (int i = 0; i < 10; i++) {
				VeGameObject obj = VeGameObject::createGameObject();
				obj.ve_model = model2;
				obj.transform.translation = {-1.0 * i * 4.0f - 4.0f, j * 4.0f, 1.0f};
				//obj.scale = {0.4f + 0.2f * j, 0.4f + 0.2f * j, 1.0f};
				//obj.color = {1.0f, 1.0f, 1.0f};
				game_objects.emplace(obj.getId(), std::move(obj));
			}
		}
		std::shared_ptr<VeModel> model3 = std::make_shared<VeModel>(ve_device, "../models/flat_vase.obj");
		for (int j = 0; j < 10; j++) {
			for (int i = 0; i < 10; i++) {
				VeGameObject obj = VeGameObject::createGameObject();
				obj.ve_model = model3;
				obj.transform = {
					.translation = {-1.0 * i * 4.0f - 8.0f, j * -4.0f - 4.0f, 0.f},
					.rotation = {glm::radians(-90.0f), 0.0f, 0.0f},
					.scale = {6.0f, 3.0f, 6.0f}
				};
				//obj.color = {1.0f, 1.0f, 1.0f};
				game_objects.emplace(obj.getId(), std::move(obj));
			}
		}
		std::shared_ptr<VeModel> model4 = std::make_shared<VeModel>(ve_device, "../models/smooth_vase.obj");
		for (int j = 0; j < 10; j++) {
			for (int i = 0; i < 10; i++) {
				VeGameObject obj = VeGameObject::createGameObject();
				obj.ve_model = model4;
				obj.transform = {
					.translation = {i * 4.0f - 4.0f, j * -4.0f - 4.0f, 0.f},
					.rotation = {glm::radians(-90.0f), 0.0f, 0.0f},
					.scale = {6.0f, 3.0f, 6.0f}
				};
				//obj.color = {1.0f, 1.0f, 1.0f};
				game_objects.emplace(obj.getId(), std::move(obj));
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

	void VeApp::createDescriptors() {
		global_pool = VeDescriptorPool::Builder(ve_device)
				.setMaxSets(MAX_FRAMES_IN_FLIGHT + MAX_FRAMES_IN_FLIGHT)
				.addPoolSize(vk::DescriptorType::eUniformBuffer, MAX_FRAMES_IN_FLIGHT)
				.addPoolSize(vk::DescriptorType::eCombinedImageSampler, MAX_FRAMES_IN_FLIGHT)
				.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
				.build();

		global_set_layout = VeDescriptorSetLayout::Builder(ve_device)
				.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics)
				.build();

		material_set_layout = VeDescriptorSetLayout::Builder(ve_device)
				.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
				.build();

		// create descriptor sets from global pool with global layout
		global_descriptor_sets.clear();
		global_descriptor_sets.reserve(static_cast<size_t>(MAX_FRAMES_IN_FLIGHT));
		for (size_t i = 0; i < static_cast<size_t>(MAX_FRAMES_IN_FLIGHT); i++) {
			auto buffer_info = uniform_buffers[i]->getDescriptorInfo();
			vk::raii::DescriptorSet set{nullptr};
			VeDescriptorWriter(*global_set_layout, *global_pool)
				.writeBuffer(0, &buffer_info)
				.build(set);
			global_descriptor_sets.push_back(std::move(set));
		}

		// Create one material descriptor set for current texture
		auto image_info = texture.getDescriptorInfo();
		material_descriptor_set = vk::raii::DescriptorSet{nullptr};
		VeDescriptorWriter(*material_set_layout, *global_pool)
			.writeImage(0, &image_info)
			.build(material_descriptor_set);
	}

	void VeApp::updateCamera() {
		// Recompute camera view once per frame if needed
		camera.updateIfDirty();
		// If swapchain aspect changed (window resize), refresh camera projection
		float aspect = ve_renderer.getExtentAspectRatio();
		if (aspect > 0.0f && std::abs(aspect - last_aspect) > std::numeric_limits<float>::epsilon()) {
			last_aspect = aspect;
			camera.setPerspective(fov, last_aspect, near_plane, far_plane);
		}
	}

	// Updates the camera and uniform buffer object once per frame
	void VeApp::updateUniformBuffer(uint32_t current_frame, UniformBufferObject& ubo) {
		ubo.view = camera.getView();
		ubo.proj = camera.getProj();
		uniform_buffers[current_frame]->writeToBuffer(&ubo);
		// No flush required with MEMORY_PROPERTY_HOST_COHERENT
	}

	void VeApp::updateWindowTitle() {
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
			snprintf(buf, sizeof(buf), "Vulkan Engine!  %d FPS  %.2f ms   location: (%.2f, %.2f, %.2f)    resolution: (%d, %d)",
							 static_cast<int>(fps), avg_ms,
							 camera.getPosition().x, camera.getPosition().y, camera.getPosition().z,
							 ve_window.getWidth(), ve_window.getHeight()
			);
			glfwSetWindowTitle(ve_window.getGLFWwindow(), buf);
			// Reset window counters
			fps_frame_count = 0;
			sum_frame_ms = 0.0;
			fps_window_start = now;
		}
	}
}

