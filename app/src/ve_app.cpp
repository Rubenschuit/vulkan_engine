#include <glm/glm.hpp>
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>

#include "ve_app.hpp"

namespace ve {

VeApp::VeApp(const std::filesystem::path& working_dir) : working_directory(working_dir) {
	// First a window, device and swap chain are initialised
	loadGameObjects();
	createUniformBuffers();
	createDescriptors();
	initSystems();
	initUI();

	// Initialise camera
	m_last_aspect = m_ve_renderer.getExtentAspectRatio();
	m_camera.setPerspective(m_fov, m_last_aspect, m_near_plane, m_far_plane);
}

VeApp::~VeApp() {}

void VeApp::run() {
	VE_LOGI("VeApp::run starting. Window=" << m_ve_window.getWidth() << "x" << m_ve_window.getHeight());

	auto current_time = std::chrono::high_resolution_clock::now();
	auto total_time = 0.0f;

	// ------------- Main loop -------------
	while (!glfwWindowShouldClose(m_ve_window.getGLFWwindow())) {
		glfwPollEvents();

		// Compute time delta for a frame
		auto new_time = std::chrono::high_resolution_clock::now();
		float dt = std::chrono::duration<float, std::chrono::seconds::period>(new_time - current_time).count();
		total_time += dt;
		// Clamp to avoid large physics steps after stalls (e.g., window resize)
		const float max_dt = 1.0f / 30.0f; // ~33ms
		if (dt < 0.0f)
			dt = 0.0f;
		if (dt > max_dt)
			dt = max_dt;

		// --------------- Begin frame ---------------

		if (!m_ve_renderer.beginFrame()) {
			current_time = new_time;
			continue;
		}
		// Next image acquired successfully
		m_frame_time = dt;
		current_time = new_time;

		// Setup frame info
		auto& command_buffer = m_ve_renderer.getCurrentCommandBuffer();
		auto& compute_command_buffer = m_ve_renderer.getCurrentComputeCommandBuffer();
		auto current_frame = m_ve_renderer.getCurrentFrame();
		VeFrameInfo frame_info{
			.global_descriptor_set = m_global_descriptor_sets[current_frame],
			.material_descriptor_set = m_material_descriptor_set,
			.cubemap_descriptor_set = m_cubemap_descriptor_set,
			.command_buffer = command_buffer,
			.compute_command_buffer = compute_command_buffer,
			.game_objects = m_game_objects,
			.frame_time = m_frame_time,
			.total_time = total_time,
			.current_frame = current_frame
		};

		// --------------- Update ---------------
		// Updates camera state based on input and frame time. Returns actions for systems.
		auto actions = m_input_controller.processInput(m_frame_time, m_camera);

		// Update state based on actions and ui_context updated in previous renderUI
		ui_context.visible = actions.ui_visible; // Tab toggles UI visibility
		updateCamera();
		updateParticles(frame_info, actions);
		updateWindowTitle();

		// update global ubo
		UniformBufferObject ubo{};
		m_point_light_system->update(frame_info, ubo);
		updateUniformBuffer(current_frame, ubo);

		// --------------- Render ---------------

		renderScene(frame_info);

		// Draw UI and update ui_context for next frame intents
		imgui_layer->renderUI(ui_context);

		// Now transition the current swapchain image to PresentSrcKHR and present the frame
		m_ve_renderer.endFrame(command_buffer);

	}
	// Ensure device is idle before destroying resources
	m_ve_device.getDevice().waitIdle();
	// log average fps and frametime over entire run currently these get reset on window resize
	VE_LOGI("VeApp::run finished. Average FPS: " << (m_fps_frame_count / (m_sum_frame_ms / 1000.0f)));
	VE_LOGI("VeApp::run finished. Average Frame Time: " << (m_sum_frame_ms / m_fps_frame_count) << " ms");
}

// Update particle system based on input actions and UI context
void VeApp::updateParticles(VeFrameInfo& frame_info, InputActions& actions) {
	// Apply input actions
	if (actions.set_mode >= 1 && actions.set_mode <= 4) {
		m_particle_system->setMode(actions.set_mode);
	}
	if (actions.reset_particles) {
		m_particle_system->resetPoint();
	} else if (actions.reset_disc) {
		m_particle_system->resetDisc();
	}

	// Apply UI intents
	m_particle_system->stageParticleCount(ui_context.pending_particle_count);
	if (ui_context.apply_particle_count) {
		m_particle_system->applyStagedParticleCount();
		ui_context.apply_particle_count = false;
	}
	if (ui_context.reset_particle_count) {
		m_particle_system->scheduleRestart();
		ui_context.reset_particle_count = false;
	}

	m_particle_system->setMean(ui_context.particle_velocity_mean);
	m_particle_system->setStddev(ui_context.particle_velocity_stddev);
	ui_context.apply_velocity_params = false; // not used currently


	// Record and submit particle compute work
	m_particle_system->update(frame_info);
	m_ve_renderer.submitCompute(frame_info.compute_command_buffer);
}

// Could be extended to render a vector of systems
void VeApp::renderScene(VeFrameInfo& frame_info) {
	auto& command_buffer = frame_info.command_buffer;
	m_ve_renderer.beginSceneRender(command_buffer);

	// systems
	m_skybox_render_system->render(frame_info);
	m_simple_render_system->renderObjects(frame_info);
	m_axes_render_system->render(frame_info);
	m_point_light_system->render(frame_info);
	m_particle_system->render(frame_info);

	m_ve_renderer.endSceneRender(command_buffer);
}

void VeApp::loadGameObjects() {
	// Create some lights with ranging colors
	constexpr uint32_t num_lights = 17; // max 100 see config
	constexpr float intensity = 0.3f;
	constexpr float radius = 1.0f;
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
	constexpr float pos_radius = 45.0f;
	constexpr float height = 10.0f;

	// Create point lights evenly distributed in a circle
	for (uint32_t i = 0; i < num_lights; i += 1) {
		auto point_light = VeGameObject::createPointLight(intensity, radius, colors[i % 10]);
		glm::vec3 pos = {
			pos_radius * cos(glm::two_pi<float>() / num_lights * i),
			pos_radius * sin(glm::two_pi<float>() / num_lights * i),
			height
		};
		point_light.transform.translation = pos;
		m_game_objects.emplace(point_light.getId(), std::move(point_light));
	}
	// 'black hole' light
	{
		auto black_hole = VeGameObject::createPointLight(1.0f, 4.0f, glm::vec3(0.0f, 0.0f, 0.0f));
		glm::vec3 pos = {0.0f, -200.0f, 10.0f};
		black_hole.transform.translation = pos;
		black_hole.point_light_component->rotates = false;
		m_game_objects.emplace(black_hole.getId(), std::move(black_hole));
	}

	// Floor
	VeGameObject floor = VeGameObject::createGameObject();
	auto quad = std::make_shared<VeModel>(m_ve_device, m_quad_model_path);
	floor.ve_model = quad;
	floor.has_texture = 0.0f;
	floor.transform = {
		.translation = {0.0f, 0.0f, -0.1f},
		.scale = {80.0f, 80.0f, 800.0f}
	};
	m_game_objects.emplace(floor.getId(), std::move(floor));

	// Textured viking rooms in a grid
	std::shared_ptr<VeModel> model = std::make_shared<VeModel>(m_ve_device, m_viking_room_model_path);
	for (int j = 0; j < 10; j++) {
		for (int i = 0; i < 10; i++) {
			VeGameObject obj = VeGameObject::createGameObject();
			obj.ve_model = model;
			obj.transform.translation = {i * 4.0f, j * 4.0f, 0.f};
			obj.has_texture = 1.0f;
			m_game_objects.emplace(obj.getId(), std::move(obj));
		}
	}

	// Cubes in a grid
	VE_LOGE("Loading cube model with path: " << m_cube_model_path);
	std::shared_ptr<VeModel> model2 = std::make_shared<VeModel>(m_ve_device, m_cube_model_path);
	for (int j = 0; j < 10; j++) {
		for (int i = 0; i < 10; i++) {
			VeGameObject obj = VeGameObject::createGameObject();
			obj.ve_model = model2;
			obj.transform.translation = {-1.0 * i * 4.0f - 4.0f, j * 4.0f, 1.0f};
			obj.transform.scale = {1.0f, 1.0f, 1.0f};
			m_game_objects.emplace(obj.getId(), std::move(obj));
		}
	}
	// Flat vases in a grid
	std::shared_ptr<VeModel> model3 = std::make_shared<VeModel>(m_ve_device, m_flat_vase_model_path);
	for (int j = 0; j < 10; j++) {
		for (int i = 0; i < 10; i++) {
			VeGameObject obj = VeGameObject::createGameObject();
			obj.ve_model = model3;
			obj.transform = {
				.translation = {-1.0 * i * 4.0f - 4.0f, j * -4.0f - 4.0f, 0.f},
				.rotation = {glm::radians(-90.0f), 0.0f, 0.0f},
				.scale = {6.0f, 3.0f, 6.0f}
			};
			m_game_objects.emplace(obj.getId(), std::move(obj));
		}
	}
	// Smooth vases in a grid
	std::shared_ptr<VeModel> model4 = std::make_shared<VeModel>(m_ve_device, m_smooth_vase_model_path);
	for (int j = 0; j < 10; j++) {
		for (int i = 0; i < 10; i++) {
			VeGameObject obj = VeGameObject::createGameObject();
			obj.ve_model = model4;
			obj.transform = {
				.translation = {i * 4.0f , j * -4.0f - 4.0f, 0.f},
				.rotation = {glm::radians(-90.0f), 0.0f, 0.0f},
				.scale = {6.0f, 3.0f, 6.0f}
			};
			m_game_objects.emplace(obj.getId(), std::move(obj));
		}
	}

}

void VeApp::createUniformBuffers() {
	vk::DeviceSize buffer_size = sizeof(UniformBufferObject);
	assert(buffer_size > 0 && "Uniform buffer size is zero");
	assert(buffer_size % 16 == 0 && "Uniform buffer size must be a multiple of 16 bytes");
	assert(buffer_size <= m_ve_device.getDeviceProperties().limits.maxUniformBufferRange && "Uniform buffer size exceeds maximum limit");

	m_uniform_buffers.clear();

	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_uniform_buffers.emplace_back(std::make_unique<VeBuffer>(
			m_ve_device,
			buffer_size,
			1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
		));
		m_uniform_buffers[i]->map();
	}
}

void VeApp::createDescriptors() {
	m_global_pool = VeDescriptorPool::Builder(m_ve_device)
		// Global sets (per-frame) + compute sets (per-frame) + material set (2) + slack
		.setMaxSets(2 * MAX_FRAMES_IN_FLIGHT + 4)
		// Uniform buffers: global (per frame) + compute (per frame)
		.addPoolSize(vk::DescriptorType::eUniformBuffer, 2 * MAX_FRAMES_IN_FLIGHT)
		// Sampler for material sets
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, 2)
		// Compute storage buffers: 2 per frame (prev + current)
		.addPoolSize(vk::DescriptorType::eStorageBuffer, 2 * MAX_FRAMES_IN_FLIGHT)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
		.buildShared();

	m_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics)
		.build();

	m_material_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();

	// create descriptor sets from global pool with global layout
	m_global_descriptor_sets.clear();
	m_global_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto buffer_info = m_uniform_buffers[i]->getDescriptorInfo();
		vk::raii::DescriptorSet set{nullptr};
		VeDescriptorWriter(*m_global_set_layout, *m_global_pool)
			.writeBuffer(0, &buffer_info)
			.build(set);
		m_global_descriptor_sets.push_back(std::move(set));
	}

	// Create one material descriptor set for current texture
	auto image_info = m_texture.getDescriptorInfo();
	m_material_descriptor_set = vk::raii::DescriptorSet{nullptr};
	VeDescriptorWriter(*m_material_set_layout, *m_global_pool)
		.writeImage(0, &image_info)
		.build(m_material_descriptor_set);

	// Create one cubemap descriptor set for skybox
	auto cubemap_image_info = m_skybox.getDescriptorInfo();
	m_cubemap_descriptor_set = vk::raii::DescriptorSet{nullptr};
	VeDescriptorWriter(*m_material_set_layout, *m_global_pool)
		.writeImage(0, &cubemap_image_info)
		.build(m_cubemap_descriptor_set);
}

void VeApp::initSystems() {
	m_simple_render_system = std::make_unique<SimpleRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		working_directory / "shaders" / "simple_shader.spv"
	);
	m_axes_render_system = std::make_unique<AxesRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		working_directory / "shaders" / "axes_shader.spv"
	);
	m_point_light_system = std::make_unique<PointLightSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		working_directory / "shaders" / "point_light_shader.spv"
	);
	m_particle_system = std::make_unique<ParticleSystem>(
		m_ve_device,
		m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		434567, // number of particles
		glm::vec3{0.0f, -200.0f, 10.0f},
		working_directory / "shaders" / "particle_compute.spv"
	);
	m_skybox_render_system = std::make_unique<SkyboxRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		working_directory / "shaders" / "skybox_shader.spv",
		working_directory / "models" / "cube.obj"
	);
}

void VeApp::initUI() {
	imgui_layer = std::make_unique<ImGuiLayer>(m_ve_window, m_ve_device, m_ve_renderer);
	ui_context = {
		.visible = false,
		.pending_particle_count = m_particle_system->getPendingParticleCount(),
		.apply_particle_count = false,
		.reset_particle_count = false,
		.particle_velocity_mean = m_particle_system->getMean(),
		.particle_velocity_stddev = m_particle_system->getStddev(),
		.apply_velocity_params = false
	};
}

// Updates the camera view and projection matrices if state changed
void VeApp::updateCamera() {
	// Recompute camera view once per frame if needed
	m_camera.updateIfDirty();
	// If swapchain aspect changed (window resize), refresh camera projection
	float aspect = m_ve_renderer.getExtentAspectRatio();
	if (aspect > 0.0f && std::abs(aspect - m_last_aspect) > std::numeric_limits<float>::epsilon()) {
		m_last_aspect = aspect;
		m_camera.setPerspective(m_fov, m_last_aspect, m_near_plane, m_far_plane);
	}
}

// Updates the camera and uniform buffer object once per frame
void VeApp::updateUniformBuffer(uint32_t current_frame, UniformBufferObject& ubo) {
	ubo.view = m_camera.getView();
	ubo.proj = m_camera.getProj();
	m_uniform_buffers[current_frame]->writeToBuffer(&ubo);
	// No flush required with MEMORY_PROPERTY_HOST_COHERENT
}

// Print FPS and frame time to window title every 100 ms
void VeApp::updateWindowTitle() {
	// Per-frame delta using steady clock
	auto now = clock::now();
	m_last_frame_ms = std::chrono::duration<double, std::milli>(now - m_last_frame_time).count();
	m_last_frame_time = now;

	// Accumulate
	m_sum_frame_ms += m_last_frame_ms;
	m_fps_frame_count++;
	auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fps_window_start).count();
	if (window_ms >= 100) { // update every 100 ms
		double fps = (window_ms > 0) ? (1000.0 * static_cast<double>(m_fps_frame_count) / static_cast<double>(window_ms)) : 0.0;
		double avg_ms = (m_fps_frame_count > 0) ? (m_sum_frame_ms / static_cast<double>(m_fps_frame_count)) : 0.0;
		char buf[128];
		// add release/debug mode with ifdef
#ifdef NDEBUG
		const char* mode_str = "Release";
#else
		const char* mode_str = "Debug";
#endif
		snprintf(buf, sizeof(buf), "Vulkan Engine! -- %s mode          FPS %d   %.2f ms",
					mode_str, static_cast<int>(fps), avg_ms
		);
		glfwSetWindowTitle(m_ve_window.getGLFWwindow(), buf);
		// Reset window counters
		m_fps_frame_count = 0;
		m_sum_frame_ms = 0.0;
		m_fps_window_start = now;
	}
}
}

