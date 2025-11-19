#include <glm/glm.hpp>
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>

#include "sandbox.hpp"
#include <imgui.h>
#include <vector>

namespace ve {

// First a window, device and swap chain are initialised in the base class
Sandbox::Sandbox(const std::filesystem::path& working_dir) : working_directory(working_dir) {

	createUniformBuffers();
	createDescriptors();

	initSystems();
	initUI();

	loadGameObjects();

	m_camera.setPerspective(m_fov, m_last_aspect, m_near_plane, m_far_plane);
}

Sandbox::~Sandbox() {}

VeFrameInfo Sandbox::update() {
	// Get frame time
	updateFrameTime();
	m_total_time += m_frame_time;


	// Setup frame info
	auto& command_buffer = m_ve_renderer.getCurrentCommandBuffer();
	auto& compute_command_buffer = m_ve_renderer.getCurrentComputeCommandBuffer();
	auto current_frame = m_ve_renderer.getCurrentFrame();
	vk::raii::DescriptorSet& material_descriptor_set =
		(ui_actions.current_scene == 2)
			? m_sponza_scene.at(sponza_id).ve_model->getMaterialDescriptorSet()
			: m_simple_material_descriptor_set;

	vk::raii::DescriptorSet& shadow_desc_set = m_shadow_render_system->getShadowDescriptorSet(current_frame);

	VeFrameInfo frame_info = {
		.global_descriptor_set = m_global_descriptor_sets[current_frame],
		.material_descriptor_set = material_descriptor_set,
		.cubemap_descriptor_set = m_cubemap_descriptor_set,
		.shadow_descriptor_set = shadow_desc_set,
		.command_buffer = command_buffer,
		.compute_command_buffer = compute_command_buffer,
		.game_objects = ui_actions.current_scene == 1 ? m_simple_scene : m_sponza_scene,
		.frame_time = m_frame_time,
		.total_time = m_total_time,
		.current_frame = current_frame
	};

	// Updates camera state based on input and frame time. Returns actions for systems.
	auto input_actions = m_input_controller.processInput(m_frame_time, m_camera);

	// Update state based on actions and ui_actions updated in previous renderUI
	ui_actions.visible = input_actions.ui_visible; // Tab toggles UI visibility
	updateCamera();
	updateParticles(frame_info, input_actions);
	updateWindowTitle();

	// update global ubo
	UniformBufferObject ubo{};
	ubo.render_mode = static_cast<uint32_t>(ui_actions.render_mode);
	ubo.shadow_mode = static_cast<uint32_t>(ui_actions.shadow_mode);
	m_point_light_system->updateUniformBuffer(frame_info, ubo); // update UBO with point light data
	m_shadow_render_system->updateUniformBuffer(current_frame, ubo);
	this->updateUniformBuffer(current_frame, ubo); // view/proj/camera location in application base class

	return frame_info;
}

// Update particle system based on input actions and UI context
// Consider moving this to the particle system class
void Sandbox::updateParticles(VeFrameInfo& frame_info, InputActions& actions) {
	// Apply input actions
	if (actions.set_mode >= 1 && actions.set_mode <= 5) {
		m_particle_system->setMode(actions.set_mode);
	}
	if (actions.reset_particles) {
		m_particle_system->resetPoint();
	} else if (actions.reset_disc) {
		m_particle_system->resetDisc();
	}

	// Apply UI inputs
	m_particle_system->stageParticleCount(ui_actions.pending_particle_count);
	if (ui_actions.apply_particle_count) {
		m_particle_system->applyStagedParticleCount();
		ui_actions.apply_particle_count = false;
	}
	if (ui_actions.reset_particle_count) {
		m_particle_system->scheduleRestart();
		ui_actions.reset_particle_count = false;
	}

	m_particle_system->setSpeed(ui_actions.speed);

	m_particle_system->setMean(ui_actions.particle_velocity_mean);
	m_particle_system->setStddev(ui_actions.particle_velocity_stddev);
	ui_actions.apply_velocity_params = false; // not used currently


	// Record and submit particle compute work
	m_particle_system->update(frame_info);
	m_ve_renderer.submitCompute(frame_info.compute_command_buffer);
}

// Renders the scene and draws the UI
void Sandbox::render(VeFrameInfo& frame_info) {
	auto& command_buffer = frame_info.command_buffer;

	// Shadow pass: render a shadow map for each light
	if (ui_actions.shadow_mode != ShadowMode::DISABLED) {
		m_shadow_render_system->renderShadowMaps(frame_info);
	}

	// Main scene pass
	m_ve_renderer.beginSceneRender(command_buffer);

	// systems
	m_skybox_render_system->render(frame_info);
	if (ui_actions.current_scene == 1) { // only render particles in simple scene TODO: make enum for scenes
		m_simple_render_system->renderObjects(frame_info);
		m_axes_render_system->render(frame_info);
		m_particle_system->render(frame_info);
	} else {
		m_pbr_render_system->renderObjects(frame_info);
	}
	m_point_light_system->render(frame_info);


	m_ve_renderer.endSceneRender(command_buffer);

	// Draw UI: begin frame, render app-specific windows, render engine windows, end frame
	m_imgui_layer->beginFrame();
	if (ui_actions.visible) {
		renderAppWindows();
		m_imgui_layer->renderEngineWindows();
	}
	m_imgui_layer->endFrame(m_ve_renderer.getCurrentCommandBuffer());
}

// Should be called between beginFrame() and endFrame() of imgui_layer
void Sandbox::renderAppWindows() {
	// Settings window with tabs
	if (ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (ImGui::BeginTabBar("SettingsTabs")) {
			if (ImGui::BeginTabItem("Scene")) {
				ImGui::Text("Scene selection");
				ImGui::Separator();
				// Select exactly 1 scene
				static int s_current_scene = ui_actions.current_scene;
				if (ImGui::RadioButton("Simple", &s_current_scene, 1))
					ui_actions.current_scene = 1;
				if (ImGui::RadioButton("Sponza", &s_current_scene, 2))
					ui_actions.current_scene = 2;


				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Particle")) {
				int count = static_cast<int>(ui_actions.pending_particle_count);
				ImGui::Text("Particle settings");
				ImGui::Separator();
				ImGui::SliderInt("Count", &count, 1000, 50000000);
				ImGui::SameLine();
				if (ImGui::Button("400k"))
					count = 400000;
				ImGui::SliderFloat("Speed", &ui_actions.speed, 0, 10);
				ImGui::SameLine();
				if (ImGui::Button("1.0x"))
					ui_actions.speed = 1.0f;
				ImGui::Separator();
				ImGui::SliderFloat("Explosion mean", &ui_actions.particle_velocity_mean, -60, 60);
				ImGui::SliderFloat("Explosion stddev", &ui_actions.particle_velocity_stddev, 0, 60);

				if (count < 1) count = 1;
				ui_actions.pending_particle_count = static_cast<uint32_t>(count);
				if (ImGui::Button("Apply count"))
					ui_actions.apply_particle_count = true;
				ImGui::SameLine();
				if (ImGui::Button("Reset"))
					ui_actions.reset_particle_count = true;
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Graphics")) {
				ImGui::Text("Graphics Settings");
				ImGui::Separator();

				// MSAA toggle
				static bool s_msaa_enabled = true;
				if (ImGui::Checkbox("Enable MSAA", &s_msaa_enabled)) {
					// TODO: recreate all pipelines
					m_ve_renderer.setMSAAEnabled(s_msaa_enabled);
				}

				// VSync toggle (eMailbox instead of eImmediate)
				static bool s_vsync_enabled = false;
				if (ImGui::Checkbox("Enable VSync", &s_vsync_enabled)) {
					m_ve_renderer.setPresentMode(s_vsync_enabled ? vk::PresentModeKHR::eMailbox : vk::PresentModeKHR::eImmediate);
				}

			// Shadow mode selection
			const char* shadow_options[] = { "Disabled", "Regular", "PCF" };
			ImGui::Combo("Shadows", &ui_actions.shadow_mode, shadow_options, 3);

				ImGui::Separator();
				ImGui::Text("Render mode");
				ImGui::Separator();
				static int s_render_mode = static_cast<int>(ui_actions.render_mode);
				if (ImGui::RadioButton("BRDF Microfacets", &s_render_mode, 5))
					ui_actions.render_mode = RenderMode::BRDF_MICROFACET;
				ImGui::SameLine();
				if (ImGui::RadioButton("BRDF Smooth", &s_render_mode, 0))
					ui_actions.render_mode = RenderMode::BRDF;
				if (ImGui::RadioButton("Normal vector", &s_render_mode, 1))
					ui_actions.render_mode = RenderMode::NORMAL_VECTOR;
				if (ImGui::RadioButton("Tangent vector", &s_render_mode, 2))
					ui_actions.render_mode = RenderMode::TANGENT_VECTOR;
				if (ImGui::RadioButton("Bitangent vector", &s_render_mode, 3))
					ui_actions.render_mode = RenderMode::BITANGENT_VECTOR;
				if (ImGui::RadioButton("Normal map", &s_render_mode, 4))
					ui_actions.render_mode = RenderMode::NORMAL_MAP;

				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::End();
	}

	// Controls window
	if (ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		// bottom right
		ImGui::SetWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 200, ImGui::GetIO().DisplaySize.y - 200), ImGuiCond_Always);
		ImGui::Text("WASD: Move");
		ImGui::Text("C: Down");
		ImGui::Text("Space: Up");
		ImGui::Separator();
		ImGui::Text("1: Earth Gravity");
		ImGui::Text("2: Cool Gravity");
		ImGui::Text("3: Succ mode");
		ImGui::Text("4: Stasis");
		ImGui::Text("5: Galaxy");
	}
	ImGui::End();
}

void Sandbox::createUniformBuffers() {
	VE_LOGD("Creating uniform buffers");
	vk::DeviceSize buffer_size = sizeof(UniformBufferObject);
	assert(buffer_size > 0 && "Uniform buffer size is zero");
	//assert(buffer_size % 16 == 0 && "Uniform buffer size must be a multiple of 16 bytes");
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

void Sandbox::createDescriptors() {
	VE_LOGD("Creating descriptors");
	m_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics)
#if ENABLE_RAY_TRACING
		.addBinding(1, vk::DescriptorType::eAccelerationStructureKHR, vk::ShaderStageFlagBits::eFragment)
#endif
		.build();

	// Shadow global descriptor set layout (for shadow pass UBO)
	m_shadow_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics)
		.build();

	m_material_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(2, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();

	// Create descriptor pool
	m_global_pool = VeDescriptorPool::Builder(m_ve_device)
		// Global sets (per-frame) + shadow global sets (per-frame, per-light) + compute sets (per-frame) + material set (3) + shadow sets (per-frame) + slack (7)
		.setMaxSets(3 * MAX_FRAMES_IN_FLIGHT + (MAX_LIGHTS * MAX_FRAMES_IN_FLIGHT) + 10 + MAX_FRAMES_IN_FLIGHT)
		// Uniform buffers: global (per frame) + shadow global (per frame, per light) + compute (per frame)
		.addPoolSize(vk::DescriptorType::eUniformBuffer, 3 * MAX_FRAMES_IN_FLIGHT + (MAX_LIGHTS * MAX_FRAMES_IN_FLIGHT))
		// Combined image samplers for material sets (3 textures per set)
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, 3 * 3)
		// Samplers: shadow sampler (1 per frame) + slack (7)
		.addPoolSize(vk::DescriptorType::eSampler, MAX_FRAMES_IN_FLIGHT + 7)
		// Sampled images: shadow map array (1 per frame) + slack (7)
		.addPoolSize(vk::DescriptorType::eSampledImage, MAX_FRAMES_IN_FLIGHT + 7)
		// Compute storage buffers: 2 per frame (prev + current)
		.addPoolSize(vk::DescriptorType::eStorageBuffer, 2 * MAX_FRAMES_IN_FLIGHT)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
		.buildShared();

	// create gloabal ubo descriptor sets (per frame)
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

	// Create simple scene material descriptor set for texture
	// TODO: maybe scene classes should own these descriptor sets?
	m_simple_albedo_texture = std::make_unique<VeTexture>(m_ve_device, m_texture_path);
	auto simple_albedo_info = m_simple_albedo_texture->getDescriptorInfo();
	m_simple_material_descriptor_set = vk::raii::DescriptorSet{nullptr};
	VeDescriptorWriter(*m_material_set_layout, *m_global_pool)
		.writeImage(0, &simple_albedo_info)
		.writeImage(1, &simple_albedo_info)
		.writeImage(2, &simple_albedo_info)
		.build(m_simple_material_descriptor_set);

	// Create one cubemap descriptor set for skybox
	auto cubemap_image_info = m_skybox.getDescriptorInfo();
	m_cubemap_descriptor_set = vk::raii::DescriptorSet{nullptr};
	VeDescriptorWriter(*m_material_set_layout, *m_global_pool)
		.writeImage(0, &cubemap_image_info)
		.build(m_cubemap_descriptor_set);
}

void Sandbox::initSystems() {
	VE_LOGD("Initialising systems");

	// Create shadow system first (before other render systems that might need shadow descriptor set)
	VE_LOGD("shadow system: " << working_directory / "shaders" / "shadow_shader.spv");
	m_shadow_render_system = std::make_unique<ShadowRenderSystem>(
		m_ve_device,
		*m_global_pool,
		m_shadow_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		working_directory / "shaders" / "shadow_shader.spv"
	);

	VE_LOGD("simple system: " << working_directory / "shaders" / "simple_shader.spv");
	m_simple_render_system = std::make_unique<SimpleRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		working_directory / "shaders" / "simple_shader.spv"
	);
	VE_LOGD("pbr system: " << working_directory / "shaders" / "pbr_shader.spv");
	m_pbr_render_system = std::make_unique<PbrRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		working_directory / "shaders" / "pbr_shader.spv"
	);
	VE_LOGD("axes system: " << working_directory / "shaders" / "axes_shader.spv");
	m_axes_render_system = std::make_unique<AxesRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		working_directory / "shaders" / "axes_shader.spv"
	);
	VE_LOGD("pl system: " << working_directory / "shaders" / "point_light_shader.spv");
	m_point_light_system = std::make_unique<PointLightSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		working_directory / "shaders" / "point_light_shader.spv"
	);
	VE_LOGD("particle system: " << working_directory / "shaders" / "particle_compute.spv");
	m_particle_system = std::make_unique<ParticleSystem>(
		m_ve_device,
		m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		500000, // number of particles
		glm::vec3{0.0f, -300.0f, 10.0f},
		working_directory / "shaders" / "particle_compute.spv"
	);
	VE_LOGD("skybox system: " << working_directory / "shaders" / "skybox_shader.spv");
	m_skybox_render_system = std::make_unique<SkyboxRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSwapChainImageFormat(),
		working_directory / "shaders" / "skybox_shader.spv",
		working_directory / "models" / "cube.gltf"
	);

}

void Sandbox::initUI() {
	VE_LOGD("Initialising UI");
	m_imgui_layer = std::make_unique<ImGuiLayer>(m_ve_window, m_ve_device, m_ve_renderer);

	ui_actions = {
		.visible = false,
		.speed = m_particle_system->getSpeed(),
		.pending_particle_count = m_particle_system->getPendingParticleCount(),
		.apply_particle_count = false,
		.reset_particle_count = false,
		.particle_velocity_mean = m_particle_system->getMean(),
		.particle_velocity_stddev = m_particle_system->getStddev(),
		.apply_velocity_params = false
	};
}

// Loads the game objects for the scene
// dont be scared of all the numbers
// Loads 2 scenes:
// 	1) A floor with simple objects in a grid and some rotating colored lights
// 	2) The sponza palace with materials and textures
void Sandbox::loadGameObjects() {

	// --------------------------------------------------------------- //
	// ----------------- Sponza scene -------------------------------- //
	// --------------------------------------------------------------- //

	glm::vec3 sponza_translation = {0.0f, 0.0f, 300.0f};

	VeGameObject sponza = VeGameObject::createGameObject();
	std::filesystem::path sponza_model_path = working_directory / "models" / "sponza" / "glTF" / "Sponza.gltf";
	auto sponza_model = std::make_shared<VeModel>(m_ve_device, sponza_model_path);
	sponza_model->createDescriptorSet(*m_global_pool, *m_material_set_layout);
	sponza.ve_model = sponza_model;
	sponza_id = sponza.getId();
	sponza.transform.translation = glm::vec3{0.0f, 0.0f, -350.0f} + sponza_translation;
	sponza.transform.scale = {0.1f, 0.1f, 0.1f};
	sponza.has_texture = 1.0f;
	m_sponza_scene.emplace(sponza.getId(), std::move(sponza));


	// sponza sun light
	VeGameObject sun = VeGameObject::createPointLight(1.0f, 4.0f, glm::vec3(1.0f, 1.0f, 1.0f));
	sun.transform.translation = glm::vec3{0.0f, 50.0f, -140.0f} + sponza_translation;
	sun.point_light_component->intensity = 100.0f;
	sun.point_light_component->rotates = true;
	m_sponza_scene.emplace(sun.getId(), std::move(sun));


	// sponza fire lights
	{
		VeGameObject fire = VeGameObject::createPointLight(1.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
		fire.transform.translation = glm::vec3{-62.0f, -22.0f, -336.0f} + sponza_translation;
		fire.point_light_component->intensity = 1.5f;
		fire.point_light_component->rotates = false;
		fire.point_light_component->casts_shadow = false;
		fire.has_shadow = false;
		m_sponza_scene.emplace(fire.getId(), std::move(fire));
	}
	{
		VeGameObject fire = VeGameObject::createPointLight(1.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
		fire.transform.translation = glm::vec3{-62.0f, 14.0f, -336.0f} + sponza_translation;
		fire.point_light_component->intensity = 1.5f;
		fire.point_light_component->rotates = false;
		fire.point_light_component->casts_shadow = false;
		fire.has_shadow = false;
		m_sponza_scene.emplace(fire.getId(), std::move(fire));
	}
	{
		VeGameObject fire = VeGameObject::createPointLight(1.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
		fire.transform.translation = glm::vec3{49.0f, 14.0f, -336.0f} + sponza_translation;
		fire.point_light_component->intensity = 1.5f;
		fire.point_light_component->rotates = false;
		fire.point_light_component->casts_shadow = false;
		fire.has_shadow = false;
		m_sponza_scene.emplace(fire.getId(), std::move(fire));
	}
	{
		VeGameObject fire = VeGameObject::createPointLight(1.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
		fire.transform.translation = glm::vec3{49.0f, -22.0f, -336.0f} + sponza_translation;
		fire.point_light_component->intensity = 1.5f;
		fire.point_light_component->rotates = false;
		fire.point_light_component->casts_shadow = false;
		fire.has_shadow = false;
		m_sponza_scene.emplace(fire.getId(), std::move(fire));
	}

	// lion eyes
	{
		VeGameObject green_eye = VeGameObject::createPointLight(1.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
		green_eye.transform.translation = glm::vec3{126.7f, -5.87f, -331.2f} + sponza_translation;
		green_eye.transform.scale = {.3f, .3f, .3f};
		green_eye.point_light_component->intensity = .1f;
		green_eye.point_light_component->rotates = false;
		green_eye.point_light_component->casts_shadow = false;
		green_eye.has_shadow = false;
		m_sponza_scene.emplace(green_eye.getId(), std::move(green_eye));
	}
	{
		VeGameObject green_eye = VeGameObject::createPointLight(1.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
		green_eye.transform.translation = glm::vec3{126.7f, -1.24f, -331.2f} + sponza_translation;
		green_eye.transform.scale = {.3f, .3f, .3f};
		green_eye.point_light_component->intensity = .1f;
		green_eye.point_light_component->rotates = false;
		green_eye.point_light_component->casts_shadow = false;
		green_eye.has_shadow = false;
		m_sponza_scene.emplace(green_eye.getId(), std::move(green_eye));
	}

	// -------------------------------------------------------------- //
	// ----------------- Simple scene ------------------------------- //
	// -------------------------------------------------------------- //

	// stationary light
	{
		auto l = VeGameObject::createPointLight(1.0f, 2.0f, glm::vec3(1.0f, 1.0f, 1.0f));
		glm::vec3 pos = {0.0f, 0.0f, 20.0f};
		l.transform.translation = pos;
		l.point_light_component->rotates = false;
		m_simple_scene.emplace(l.getId(), std::move(l));
	}

	// Create some lights with ranging colors
	constexpr uint32_t num_lights = 2; // max 100 see config
	constexpr float intensity = 0.7f;
	constexpr float radius = 1.0f;
	const glm::vec3 colors[10] = {
		{0.0f, 1.0f, 1.0f}, //cyan
		{1.0f, 0.0f, 0.0f}, //red
		{1.0f, 0.5f, 0.0f}, //orange
		{1.0f, 1.0f, 0.0f}, //yellow
		{0.0f, 1.0f, 0.0f}, //green
		{0.0f, 1.0f, 0.5f}, //turquoise
		{1.0f, 1.0f, 1.0f}, //white
		{0.0f, 0.5f, 1.0f}, //light-blue
		{0.0f, 0.0f, 1.0f}, //blue
		{0.5f, 0.0f, 1.0f}  //purple
	};
	constexpr float pos_radius = 28.0f;
	constexpr float height = 20.0f;

	// Create point lights evenly distributed in a circle
	for (uint32_t i = 0; i < num_lights; i += 1) {
		auto point_light = VeGameObject::createPointLight(intensity, radius, colors[i % 10]);
		glm::vec3 pos = {
			pos_radius * cos(glm::two_pi<float>() / num_lights * (float)i),
			pos_radius * sin(glm::two_pi<float>() / num_lights * (float)i),
			height
		};
		point_light.has_shadow = false;
		point_light.transform.translation = pos;
		m_simple_scene.emplace(point_light.getId(), std::move(point_light));
	}
	/*
	// 'black hole' light
	{
		auto black_hole = VeGameObject::createPointLight(1.0f, 4.0f, glm::vec3(0.0f, 0.0f, 0.0f));
		glm::vec3 pos = {0.0f, -300.0f, 10.0f};
		black_hole.transform.translation = pos;
		black_hole.point_light_component->rotates = false;
		m_simple_scene.emplace(black_hole.getId(), std::move(black_hole));
		m_sponza_scene.emplace(black_hole.getId(), std::move(black_hole));
	}
	*/



	// --------------------------------------------------------------
	// floor
	// --------------------------------------------------------------

	VeGameObject floor = VeGameObject::createGameObject();
	auto quad = std::make_shared<VeModel>(m_ve_device, m_quad_model_path);
	floor.ve_model = quad;
	floor.has_texture = 0.0f;
	floor.transform = {
		.translation = {0.0f, 0.0f, 0.0f},
		.rotation = {glm::radians(90.0f), 0.0f, 0.0f},
		.scale = {80.0f, 1.0f, 80.0f}
	};
	floor.has_shadow = false;
	m_simple_scene.emplace(floor.getId(), std::move(floor));


	// --------------------------------------------------------------
	// textured quad
	// --------------------------------------------------------------

	VeGameObject textured_quad = VeGameObject::createGameObject();
	auto textured_quad_model = std::make_shared<VeModel>(m_ve_device, m_quad_model_path);
	textured_quad.ve_model = quad;
	textured_quad.has_texture = 1.0f;
	textured_quad.transform = {
		.translation = {-30.0f, -30.0f, 40.0f},
		.rotation = {0.0f, 0.0f, glm::radians(-45.0f)},
		.scale = {20.0f, 1.0f, 20.0f}
	};
	m_simple_scene.emplace(textured_quad.getId(), std::move(textured_quad));
	// --------------------------------------------------------------
	// viking rooms
	// --------------------------------------------------------------

	// Spheres in a grid
	std::shared_ptr<VeModel> model = std::make_shared<VeModel>(m_ve_device, m_sphere_model_path);
	for (int j = 0; j < 5; j++) {
		for (int i = 0; i < 5; i++) {
			VeGameObject obj = VeGameObject::createGameObject();
			obj.ve_model = model;
			obj.transform.rotation = {0.0f, 0.0f, 0.0f};
			obj.transform.translation = {(float)i * 4.0f, (float)j * 4.0f, 1.0f};
			obj.transform.scale = {1.0f, 1.0f, 1.0f};
			obj.has_texture = 0.0f;
			m_simple_scene.emplace(obj.getId(), std::move(obj));
		}
	}

	// --------------------------------------------------------------
	// cubes
	// --------------------------------------------------------------

	// Cubes in a grid
	std::shared_ptr<VeModel> model2 = std::make_shared<VeModel>(m_ve_device, m_cube_model_path);
	for (int j = 0; j < 5; j++) {
		for (int i = 0; i < 5; i++) {
			VeGameObject obj = VeGameObject::createGameObject();
			obj.ve_model = model2;
			obj.transform.translation = {-1.0 * (float)i * 4.0f - 4.0f, (float)j * 4.0f, 1.0f};
			obj.transform.scale = {1.0f, 1.0f, 1.0f};
			obj.has_texture = 0.0f;
			m_simple_scene.emplace(obj.getId(), std::move(obj));
		}
	}

	// --------------------------------------------------------------
	// Vases
	// --------------------------------------------------------------

	// Flat vases (bad normals) in a grid
	std::shared_ptr<VeModel> model3 = std::make_shared<VeModel>(m_ve_device, m_flat_vase_model_path);
	for (int j = 0; j < 5; j++) {
		for (int i = 0; i < 5; i++) {
			VeGameObject obj = VeGameObject::createGameObject();
			obj.ve_model = model3;
			obj.transform = {
				.translation = {-1.0 * (float)i * 4.0f - 4.0f, (float)j * -4.0f - 4.0f, 0.f},
				.rotation = {glm::radians(-180.0f), 0.0f, 0.0f},
				.scale = {6.0f, 6.0f, 3.0f}
			};
			obj.has_texture = 0.0f;
			m_simple_scene.emplace(obj.getId(), std::move(obj));
		}
	}
	// Smooth vases (interpolated normals) in a grid
	std::shared_ptr<VeModel> model4 = std::make_shared<VeModel>(m_ve_device, m_smooth_vase_model_path);
	for (int j = 0; j < 5; j++) {
		for (int i = 0; i < 5; i++) {
			VeGameObject obj = VeGameObject::createGameObject();
			obj.ve_model = model4;
			obj.transform = {
				.translation = {(float)i * 4.0f , (float)j * -4.0f - 4.0f, 0.f},
				.rotation = {glm::radians(-180.0f), 0.0f, 0.0f},
				.scale = {6.0f, 6.0f, 3.0f}
			};
			m_simple_scene.emplace(obj.getId(), std::move(obj));
		}
	}

}


} // namespace ve

// Called by the entry point to create the application instance
ve::VeApplication* createApp(std::filesystem::path working_directory) {
	return new ve::Sandbox(working_directory);
}

