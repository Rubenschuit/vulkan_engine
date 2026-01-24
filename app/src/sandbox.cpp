#include <glm/glm.hpp>
#define GLM_FORCE_RADIANS
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>

#include "sandbox.hpp"
#include "utils/ve_random.hpp"
#include <imgui.h>
#include <vector>
#include <cstdlib>

namespace ve {

// First a window, device and swap chain are initialised in the base class
Sandbox::Sandbox(const std::filesystem::path& working_dir) : project_root(working_dir) {
	// Initialize scenes
	m_simple_scene = std::make_unique<VeScene>("Simple Scene");
	m_sponza_scene = std::make_unique<VeScene>("Sponza Scene");

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

	// TODO move to scene class
	vk::raii::DescriptorSet& material_descriptor_set =
		(ui_actions.current_scene == 2)
			? m_sponza_scene->getGameObjects().at(sponza_id).ve_model->getMaterialDescriptorSet()
			: m_texture_descriptor_set;

	vk::raii::DescriptorSet& shadow_desc_set = m_shadow_render_system->getShadowDescriptorSet(current_frame);

	int color_space_type = 0; // default SRGB
	auto swapchain_color_space = m_ve_renderer.getSwapChainColorSpace();
	if (swapchain_color_space == vk::ColorSpaceKHR::eExtendedSrgbLinearEXT) {
		color_space_type = 1;
	} else if (swapchain_color_space == vk::ColorSpaceKHR::eHdr10St2084EXT) {
		color_space_type = 2;
	}

	VeFrameInfo frame_info = {
		.global_descriptor_set = m_global_descriptor_sets[current_frame],
		.texture_descriptor_set = m_texture_descriptor_set,
		.material_descriptor_set = material_descriptor_set,
		.cubemap_descriptor_set = m_cubemap_descriptor_set,
		.shadow_descriptor_set = shadow_desc_set,
		.command_buffer = command_buffer,
		.compute_command_buffer = compute_command_buffer,
		.camera = m_camera,
		.game_objects = ui_actions.current_scene == 1 ? m_simple_scene->getGameObjects() : m_sponza_scene->getGameObjects(),
		.frame_time = m_frame_time,
		.total_time = m_total_time,
		.current_frame = current_frame,
		.post_process_push = {
			ui_actions.blur_radius,
			ui_actions.blur_strength,
			ui_actions.exposure,
			color_space_type,
			ui_actions.bloom_enabled ? ui_actions.bloom_strength : 0.0f
		}
	};

	// Updates camera state based on input and frame time. Returns actions for systems.
	auto input_actions = m_input_controller.processInput(m_frame_time, m_camera);

	// Update state based on actions and ui_actions updated in previous renderUI
	// Tab toggles "UI Mode" (cursor visible). Settings window is ONLY visible in UI mode.
	ui_actions.visible = input_actions.ui_visible;

	// P key toggles performance and controls windows
	if (input_actions.toggle_performance_ui) {
		ui_actions.show_performance = !ui_actions.show_performance;
		ui_actions.show_controls = !ui_actions.show_controls;
	}

	updateCamera();
	updateParticles(input_actions);
	updateWindowTitle();

	// update sponza sun intensity
	if (ui_actions.current_scene == 2) {
		auto& sponza_objects = m_sponza_scene->getGameObjects();
		if (sponza_objects.contains(sun_id)) {
			sponza_objects.at(sun_id).point_light_component->intensity = ui_actions.sun_intensity;
		}
	}

	// update ubos
	UniformBufferObject ubo{};
	ubo.render_mode = static_cast<uint32_t>(ui_actions.render_mode);
	ubo.shadow_mode = static_cast<uint32_t>(ui_actions.shadow_mode);
	m_point_light_system->updateUniformBuffer(frame_info, ubo); // update UBO with point light data
	m_shadow_render_system->updateUniformBuffer(current_frame, ubo); // update internal shadow UBO with light data from main UBO
	this->updateUniformBuffer(current_frame, ubo); // view/proj/camera location in application base class

	// Record and submit compute work (two particle systems)
	m_fireworks_system->recordComputeCommands(frame_info);
	m_particle_system->recordComputeCommands(frame_info);

	frame_info.compute_command_buffer.end();
	m_ve_renderer.submitCompute(frame_info.compute_command_buffer);

	return frame_info;
}

// Update particle system based on input actions and UI context
// Consider moving this to the particle system class
void Sandbox::updateParticles(InputActions& actions) {
	// Apply input actions
	if (actions.set_mode >= 1 && actions.set_mode <= 5) {
		m_particle_system->setMode(actions.set_mode);
		ui_actions.current_mode = static_cast<int>(actions.set_mode);
	}
	if (actions.reset_particles) {
		m_particle_system->setOrigin(m_camera.getForward() * 100.0f + m_camera.getPosition());
		m_particle_system->resetPoint();
	} else if (actions.reset_disc) {
		m_particle_system->setOrigin(m_camera.getForward() * 100.0f + m_camera.getPosition());
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
	m_particle_system->setLifeRange(ui_actions.min_life, ui_actions.max_life);
	m_particle_system->setShouldRespawn(ui_actions.should_respawn);
	if (actions.launch_firework) {
		m_fireworks_system->launchRocket();
	}
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
		if (ui_actions.show_axes) {
			m_axes_render_system->render(frame_info);
		}
		m_particle_system->render(frame_info);
	} else {
		m_pbr_render_system->renderObjects(frame_info);
	}
	m_point_light_system->render(frame_info);
	m_fireworks_system->render(frame_info);


	m_ve_renderer.endSceneRender(command_buffer);

	// Bloom pass
	if (ui_actions.bloom_enabled) {
		m_bloom_system->render(command_buffer);
	}

	// Post-processing pass
	m_ve_renderer.beginPostProcessRender(command_buffer);
	m_post_process_system->render(command_buffer, frame_info.post_process_push);
	m_ve_renderer.endPostProcessRender(command_buffer);

	// Draw UI: begin frame, render app-specific windows, render engine windows, end frame
	m_imgui_layer->beginFrame();
	if (ui_actions.visible) {
		renderAppWindows();
	}
	m_imgui_layer->renderEngineWindows(ui_actions);
	renderControlsWindow();
	m_imgui_layer->endFrame(m_ve_renderer.getCurrentCommandBuffer());
}

void Sandbox::onSwapChainRecreated() {
	recreatePipelines();
}

void Sandbox::recreatePipelines() {
	m_ve_device.getDevice().waitIdle();
	auto color_format = m_ve_renderer.getSwapChainImageFormat();
	auto offscreen_format = m_ve_renderer.getOffscreenImageFormat();
	auto sample_count = m_ve_renderer.getSampleCount();
	auto extent = m_ve_renderer.getExtent();

	m_bloom_system->recreateResources(extent, m_ve_renderer.getResolveTargetImageView());

	m_simple_render_system->recreatePipeline(offscreen_format, sample_count);
	m_point_light_system->recreatePipeline(offscreen_format, sample_count);
	m_pbr_render_system->recreatePipeline(offscreen_format, sample_count);
	m_axes_render_system->recreatePipeline(offscreen_format, sample_count);
	m_skybox_render_system->recreatePipeline(offscreen_format, sample_count);
	m_particle_system->recreatePipeline(offscreen_format, sample_count);
	m_fireworks_system->recreatePipeline(offscreen_format, sample_count);

	m_post_process_system->recreatePipeline(color_format, m_ve_renderer.getResolveTargetImageView(), m_bloom_system->getBloomTexture());
	m_imgui_layer->recreatePipeline();
	// Shadow render system pipeline does not depend on swap chain MSAA
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

				if (ui_actions.current_scene == 2) {
					ImGui::Separator();
					ImGui::Text("Sponza Settings");
					ImGui::SliderFloat("Sun Intensity", &ui_actions.sun_intensity, 0.0f, 1000000.0f);
				}


				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Particle")) {
				int count = static_cast<int>(ui_actions.pending_particle_count);

				// 1. Simulation Control
				ImGui::Text("Simulation Control");
				ImGui::Separator();
				ImGui::SliderInt("Active Count", &count, 1000, 5000000);
				if (count < 1) count = 1;
				ui_actions.pending_particle_count = static_cast<uint32_t>(count);
				if (ImGui::Button("Apply Count"))
					ui_actions.apply_particle_count = true;

				ImGui::SameLine();
				if (ImGui::Button("Reset System"))
					ui_actions.reset_particle_count = true;

				ImGui::SliderFloat("Speed", &ui_actions.speed, 0, 10);
				ImGui::SameLine();
				if (ImGui::Button("1.0x")) ui_actions.speed = 1.0f;

				// 3. Lifetime
				ImGui::Separator();
				ImGui::Text("Lifetime");
				ImGui::Separator();
				ImGui::SliderFloat("Min Life", &ui_actions.min_life, 0.1f, 100.0f);
				ImGui::SliderFloat("Max Life", &ui_actions.max_life, 0.1f, 100.0f);
				ImGui::Checkbox("Respawn", &ui_actions.should_respawn);
				// Ensure min <= max
				if (ui_actions.min_life > ui_actions.max_life) ui_actions.min_life = ui_actions.max_life;

				// 4. Physics
				ImGui::Separator();
				ImGui::Text("Physics / Explosion");
				ImGui::Separator();
				ImGui::SliderFloat("Mean Velocity", &ui_actions.particle_velocity_mean, -60, 60);
				ImGui::SliderFloat("StdDev Velocity", &ui_actions.particle_velocity_stddev, 0, 60);


				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Fireworks")) {
				auto& config = m_fireworks_system->getConfig();

				ImGui::Text("Launch Settings");
				ImGui::Separator();
				ImGui::DragFloat3("Launch Position", &config.launch_pos.x);
				ImGui::DragFloatRange2("Launch Velocity", &config.launch_vel_min, &config.launch_vel_max, 1.0f, 0.0f, 500.0f);
				ImGui::SliderFloat("Spread XY", &config.launch_spread_xy, 0.0f, 100.0f);
				ImGui::SliderInt("Launch Count", &config.launch_count, 1, 100);

				ImGui::Text("Colors");
				ImGui::Checkbox("Random Color", &config.use_random_color);
				if (!config.use_random_color) {
					ImGui::ColorEdit4("Particle Color", &config.particle_color.r);
				}

				ImGui::Separator();
				ImGui::Text("Explosion");
				ImGui::SliderInt("Particle Count", &config.explosion_particle_count, 5, 3000);
				ImGui::SliderFloat("Explosion Size", &config.explosion_size, 0.1f, 5.0f);
				ImGui::SliderFloat("Trail Interval", &config.trail_interval, 0.0001f, 0.1f, "%.4f");

				ImGui::Separator();
				ImGui::Text("Environment");
				ImGui::DragFloat3("Wind Direction", &config.wind_direction.x, 0.05f, -1.0f, 1.0f);
				ImGui::SliderFloat("Wind Strength", &config.wind_strength, 0.0f, 50.0f);
				ImGui::SliderFloat("Gravity", &config.gravity, 0.0f, 50.0f);

				ImGui::Separator();
				ImGui::Text("System");
				ImGui::SliderInt("Particle Capacity", &config.max_particles, 1000, 5000000);
				if (ImGui::Button("Apply Capacity")) {
					m_fireworks_system->setParticleCapacity(static_cast<uint32_t>(config.max_particles));
				}

				if (ImGui::Button("Launch Rocket", ImVec2(-1, 0))) {
					m_fireworks_system->launchRocket();
				}

				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Graphics")) {
				ImGui::Text("Graphics Settings");
				ImGui::Separator();

				// MSAA toggle
				static bool s_msaa_enabled = true;
				if (ImGui::Checkbox("Enable MSAA (device maximum sample count)", &s_msaa_enabled)) {
					m_ve_renderer.setMSAAEnabled(s_msaa_enabled);
				}

				// VSync toggle (eMailbox instead of eImmediate)
				static bool s_vsync_enabled = false;
				if (ImGui::Checkbox("Enable VSync", &s_vsync_enabled)) {
					m_ve_renderer.setPresentMode(s_vsync_enabled ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate);
				}

				// HDR toggle
				bool hdr_supported = m_ve_renderer.hasHdrSupport();
				if (!hdr_supported) {
					ImGui::BeginDisabled();
				}
				if (ImGui::Checkbox("Enable HDR", &ui_actions.hdr_enabled)) {
					m_ve_renderer.setHdrEnabled(ui_actions.hdr_enabled);
				}
				if (hdr_supported && ui_actions.hdr_enabled) {
					auto color_space = m_ve_renderer.getSwapChainColorSpace();
					std::string mode_str = "Selected Mode: ";
					if (color_space == vk::ColorSpaceKHR::eHdr10St2084EXT) mode_str += "HDR10 (PQ)";
					else if (color_space == vk::ColorSpaceKHR::eExtendedSrgbLinearEXT) mode_str += "Extended sRGB (scRGB)";
					else mode_str += "Standard (SDR)";
					ImGui::SameLine();
					ImGui::TextDisabled("| %s", mode_str.c_str());
				}
				if (!hdr_supported) {
					ImGui::EndDisabled();
					ImGui::SameLine();
					ImGui::TextDisabled("(Not supported by device)");
				}

				// Shadow mode selection
				ImGui::Text("Shadows: ");
				ImGui::SameLine();
				const char* shadow_options[] = { "Disabled", "Regular", "PCF" };
				ImGui::Combo(" ", &ui_actions.shadow_mode, shadow_options, 3);

				// Topology selection. Requires pipeline recreation
				ImGui::Text("Topology: ");
				ImGui::SameLine();
				if (ImGui::RadioButton("Triangle List", &ui_actions.topology, 0)) {
					ui_actions.topology = Topology::TRIANGLE_LIST;
					m_simple_render_system->setTopology(vk::PrimitiveTopology::eTriangleList);
					m_pbr_render_system->setTopology(vk::PrimitiveTopology::eTriangleList);
					m_ve_renderer.setSwapChainNeedsRecreation();
				}
				ImGui::SameLine();
				if (ImGui::RadioButton("Line List", &ui_actions.topology, 1)) {
					ui_actions.topology = Topology::LINE_LIST;
					m_simple_render_system->setTopology(vk::PrimitiveTopology::eLineList);
					m_pbr_render_system->setTopology(vk::PrimitiveTopology::eLineList);
					m_ve_renderer.setSwapChainNeedsRecreation();
				}


				ImGui::Separator();
				ImGui::Text("Render mode (sponza): ");
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

				// Axes system visibility
				ImGui::Separator();
				ImGui::Checkbox("Show Axes", &ui_actions.show_axes);

				ImGui::Separator();
				ImGui::Text("Post Processing: ");
				ImGui::SliderInt("Blur Radius", &ui_actions.blur_radius, 0, 10);
				ImGui::SliderFloat("Blur Strength", &ui_actions.blur_strength, 0.0f, 5.0f);
				ImGui::SliderFloat("Exposure", &ui_actions.exposure, 0.0f, 5.0f);

				ImGui::Text("Bloom");
				ImGui::Separator();
				ImGui::Checkbox("Bloom Enabled", &ui_actions.bloom_enabled);
				ImGui::DragFloat("Bloom Strength", &ui_actions.bloom_strength, 0.001f, 0.0f, 0.4f);

				ImGui::EndTabItem();
			}

		}
		ImGui::EndTabBar();
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

	// Global set layout for global UBO and optional ray tracing acceleration structure
	m_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics)
#if ENABLE_RAY_TRACING
		.addBinding(1, vk::DescriptorType::eAccelerationStructureKHR, vk::ShaderStageFlagBits::eFragment)
#endif
		.build();

	// Material set layout for three texture samplers (albedo, normal, roughness for example)
	m_material_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(2, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();

	// Create descriptor pool
	m_global_pool = VeDescriptorPool::Builder(m_ve_device)
		// Global sets (per-frame) + shadow global sets (per-frame, per-light) + compute sets (per-frame) + material set (3) + shadow sets (per-frame) + slack (7)
		.setMaxSets(4 * MAX_FRAMES_IN_FLIGHT + (MAX_LIGHTS * MAX_FRAMES_IN_FLIGHT) + 10 + MAX_FRAMES_IN_FLIGHT)
		// Uniform buffers: global (per frame) + shadow global (per frame, per light) + compute (2x per frame)
		.addPoolSize(vk::DescriptorType::eUniformBuffer, 4 * MAX_FRAMES_IN_FLIGHT + (MAX_LIGHTS * MAX_FRAMES_IN_FLIGHT))
		// Combined image samplers for material sets (3 textures per set)
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, 3 * 3)
		// Samplers: shadow sampler (1 per frame) + slack (7)
		.addPoolSize(vk::DescriptorType::eSampler, MAX_FRAMES_IN_FLIGHT + 7)
		// Sampled images: shadow map array (1 per frame) + slack (7)
		.addPoolSize(vk::DescriptorType::eSampledImage, MAX_FRAMES_IN_FLIGHT + 7)
		// Compute storage buffers:
		.addPoolSize(vk::DescriptorType::eStorageBuffer, 14 * MAX_FRAMES_IN_FLIGHT)
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
	m_glow_texture = std::make_unique<VeTexture>(m_ve_device, m_glow_texture_path);
	m_fire_texture = std::make_unique<VeTexture>(m_ve_device, m_fire_texture_path);
	m_smoke_texture = std::make_unique<VeTexture>(m_ve_device, m_smoke_texture_path);
	auto glow_texture_info = m_glow_texture->getDescriptorInfo();
	auto fire_texture_info = m_fire_texture->getDescriptorInfo();
	auto smoke_texture_info = m_smoke_texture->getDescriptorInfo();
	m_texture_descriptor_set = vk::raii::DescriptorSet{nullptr};
	VeDescriptorWriter(*m_material_set_layout, *m_global_pool)
		.writeImage(0, &glow_texture_info)
		.writeImage(1, &fire_texture_info)
		.writeImage(2, &smoke_texture_info)
		.build(m_texture_descriptor_set);

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
	VE_LOGD("shadow system: " << project_root / "shaders" / "shadow_shader.spv");
	m_shadow_render_system = std::make_unique<ShadowRenderSystem>(
		m_ve_device,
		*m_global_pool,
		m_material_set_layout->getDescriptorSetLayout(),
		project_root / "shaders" / "shadow_shader.spv"
	);

	VE_LOGD("simple system: " << project_root / "shaders" / "simple_shader.spv");
	m_simple_render_system = std::make_unique<SimpleRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		project_root / "shaders" / "simple_shader.spv"
	);
	VE_LOGD("pbr system: " << project_root / "shaders" / "pbr_shader.spv");
	m_pbr_render_system = std::make_unique<PbrRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		project_root / "shaders" / "pbr_shader.spv"
	);
	VE_LOGD("axes system: " << project_root / "shaders" / "axes_shader.spv");
	m_axes_render_system = std::make_unique<AxesRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		project_root / "shaders" / "axes_shader.spv"
	);
	VE_LOGD("pl system: " << project_root / "shaders" / "point_light_shader.spv");
	m_point_light_system = std::make_unique<PointLightSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		project_root / "shaders" / "point_light_shader.spv"
	);
	VE_LOGD("particle system: " << project_root / "shaders" / "particle_compute.spv");
	m_particle_system = std::make_unique<ParticleSystem>(
		m_ve_device,
		m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		50000, // number of particles
		glm::vec3{0.0f, -300.0f, 50.0f},
		project_root / "shaders" / "particle_compute.spv"
	);
	m_fireworks_system = std::make_unique<FireworksSystem>(
		m_ve_device,
		m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		project_root / "shaders" / "particle_compute.spv"
	);
	VE_LOGD("skybox system: " << project_root / "shaders" / "skybox_shader.spv");
	m_skybox_render_system = std::make_unique<SkyboxRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		project_root / "shaders" / "skybox_shader.spv",
		project_root / "models" / "cube.gltf"
	);

	m_bloom_system = std::make_unique<BloomSystem>(
		m_ve_device,
		m_ve_renderer.getExtent(),
		m_ve_renderer.getResolveTargetImageView(),
		project_root / "shaders" / "bloom_downsample.spv",
		project_root / "shaders" / "bloom_upsample.spv"
	);

	m_post_process_system = std::make_unique<PostProcessSystem>(
		m_ve_device,
		m_ve_renderer.getSwapChainImageFormat(),
		m_ve_renderer.getResolveTargetImageView(),
		m_bloom_system->getBloomTexture(),
		project_root / "shaders" / "post_process.spv"
	);
}

void Sandbox::initUI() {
	VE_LOGD("Initialising UI");
	m_imgui_layer = std::make_unique<ImGuiLayer>(m_ve_window, m_ve_device, m_ve_renderer);

	ui_actions = UIContext{};
	ui_actions.hdr_enabled = m_ve_renderer.hasHdrSupport() && m_ve_renderer.isHdrEnabled();
	ui_actions.speed = m_particle_system->getSpeed();
	ui_actions.pending_particle_count = m_particle_system->getPendingParticleCount();
	ui_actions.particle_velocity_mean = m_particle_system->getMean();
	ui_actions.particle_velocity_stddev = m_particle_system->getStddev();
	ui_actions.min_life = m_particle_system->getMinLife();
	ui_actions.max_life = m_particle_system->getMaxLife();
	ui_actions.should_respawn = m_particle_system->getShouldRespawn();
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
	std::filesystem::path sponza_model_path = project_root / "models" / "sponza" / "glTF" / "Sponza.gltf";
	auto sponza_model = std::make_shared<VeModel>(m_ve_device, sponza_model_path);
	sponza_model->createDescriptorSet(*m_global_pool, *m_material_set_layout);
	sponza.ve_model = sponza_model;
	sponza_id = sponza.getId();
	sponza.transform.translation = glm::vec3{0.0f, 0.0f, -350.0f} + sponza_translation;
	sponza.transform.scale = {0.1f, 0.1f, 0.1f};
	sponza.has_texture = 1.0f;
	m_sponza_scene->getGameObjects().emplace(sponza.getId(), std::move(sponza));


	// sponza sun light
	VeGameObject sun = VeGameObject::createPointLight(2000.0f, 4.0f, glm::vec3(1.0f, 1.0f, 1.0f));
	sun.transform.translation = glm::vec3{0.0f, 50.0f, -140.0f} + sponza_translation;
	sun.point_light_component->rotates = true;
	sun_id = sun.getId();
	m_sponza_scene->getGameObjects().emplace(sun.getId(), std::move(sun));

	// sponza fire lights
	{
		VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
		fire.transform.translation = glm::vec3{-62.0f, -22.0f, -336.0f} + sponza_translation;
		fire.point_light_component->rotates = false;
		fire.point_light_component->casts_shadow = false;
		fire.has_shadow = false;
		m_sponza_scene->getGameObjects().emplace(fire.getId(), std::move(fire));
	}
	{
		VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
		fire.transform.translation = glm::vec3{-62.0f, 14.0f, -336.0f} + sponza_translation;
		fire.point_light_component->rotates = false;
		fire.point_light_component->casts_shadow = false;
		fire.has_shadow = false;
		m_sponza_scene->getGameObjects().emplace(fire.getId(), std::move(fire));
	}
	{
		VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
		fire.transform.translation = glm::vec3{49.0f, 14.0f, -336.0f} + sponza_translation;
		fire.point_light_component->rotates = false;
		fire.point_light_component->casts_shadow = false;
		fire.has_shadow = false;
		m_sponza_scene->getGameObjects().emplace(fire.getId(), std::move(fire));
	}
	{
		VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
		fire.transform.translation = glm::vec3{49.0f, -22.0f, -336.0f} + sponza_translation;
		fire.point_light_component->rotates = false;
		fire.point_light_component->casts_shadow = false;
		fire.has_shadow = false;
		m_sponza_scene->getGameObjects().emplace(fire.getId(), std::move(fire));
	}

	// lion eyes
	{
		VeGameObject green_eye = VeGameObject::createPointLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
		green_eye.transform.translation = glm::vec3{126.7f, -5.87f, -331.2f} + sponza_translation;
		green_eye.transform.scale = {.3f, .3f, .3f};
		green_eye.point_light_component->rotates = false;
		green_eye.point_light_component->casts_shadow = false;
		green_eye.has_shadow = false;
		m_sponza_scene->getGameObjects().emplace(green_eye.getId(), std::move(green_eye));
	}
	{
		VeGameObject green_eye = VeGameObject::createPointLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
		green_eye.transform.translation = glm::vec3{126.7f, -1.24f, -331.2f} + sponza_translation;
		green_eye.transform.scale = {.3f, .3f, .3f};
		green_eye.point_light_component->rotates = false;
		green_eye.point_light_component->casts_shadow = false;
		green_eye.has_shadow = false;
		m_sponza_scene->getGameObjects().emplace(green_eye.getId(), std::move(green_eye));
	}

	// -------------------------------------------------------------- //
	// ----------------- Simple scene ------------------------------- //
	// -------------------------------------------------------------- //

	// stationary light
	{
		auto l = VeGameObject::createPointLight(100.0f, 2.0f, glm::vec3(1.0f, 1.0f, 1.0f));
		glm::vec3 pos = {0.0f, 0.0f, 20.0f};
		l.transform.translation = pos;
		l.point_light_component->rotates = false;
		m_simple_scene->getGameObjects().emplace(l.getId(), std::move(l));
	}

	// Create some lights with ranging colors
	constexpr uint32_t num_lights = 7; // max 100 see config
	constexpr float intensity = 100.0f;
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
		point_light.point_light_component->casts_shadow = false;
		point_light.has_shadow = false;
		point_light.transform.translation = pos;
		m_simple_scene->getGameObjects().emplace(point_light.getId(), std::move(point_light));
	}

	// 'black hole' light
	{
		auto black_hole = VeGameObject::createPointLight(1.0f, 4.0f, glm::vec3(0.0f, 0.0f, 0.0f));
		glm::vec3 pos = {0.0f, -300.0f, 10.0f};
		black_hole.transform.translation = pos;
		black_hole.point_light_component->rotates = false;
		black_hole.point_light_component->casts_shadow = false;
		black_hole.has_shadow = false;
		m_simple_scene->getGameObjects().emplace(black_hole.getId(), std::move(black_hole));
		m_sponza_scene->getGameObjects().emplace(black_hole.getId(), std::move(black_hole));
	}




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
	m_simple_scene->getGameObjects().emplace(floor.getId(), std::move(floor));


	// --------------------------------------------------------------
	// textured quad to display certain textures or shadow maps
	// --------------------------------------------------------------

	/*
	VeGameObject textured_quad = VeGameObject::createGameObject();
	auto textured_quad_model = std::make_shared<VeModel>(m_ve_device, m_quad_model_path);
	textured_quad.ve_model = quad;
	textured_quad.has_texture = 1.0f;
	textured_quad.has_shadow = false;
	textured_quad.transform = {
		.translation = {-30.0f, -30.0f, 40.0f},
		.rotation = {0.0f, 0.0f, glm::radians(-45.0f)},
		.scale = {20.0f, 1.0f, 20.0f}
	};
	m_simple_scene->getGameObjects().emplace(textured_quad.getId(), std::move(textured_quad));
	*/
	// --------------------------------------------------------------
	// Spheres
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
			m_simple_scene->getGameObjects().emplace(obj.getId(), std::move(obj));
		}
	}

	// --------------------------------------------------------------
	// Cubes
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
			m_simple_scene->getGameObjects().emplace(obj.getId(), std::move(obj));
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
			m_simple_scene->getGameObjects().emplace(obj.getId(), std::move(obj));
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
			m_simple_scene->getGameObjects().emplace(obj.getId(), std::move(obj));
		}
	}

}


void Sandbox::renderControlsWindow() {
	if (!ui_actions.show_controls) return;

	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 10.0f, ImGui::GetIO().DisplaySize.y - 10.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;
	// If UI is visible (mouse unlocked), allow interaction and decoration (close button)
	if (ui_actions.visible) {
		flags = ImGuiWindowFlags_AlwaysAutoResize;
	}

	if (ImGui::Begin("Controls", &ui_actions.show_controls, flags)) {
		ImGui::Text("WASD: Move | Space/C: Up/Down | Shift: Sprint");
		ImGui::Separator();
		ImGui::Text("Particle simulation mode: ");
		const char* modes[] = { "Earth Gravity", "Cool Gravity", "Succ mode", "Stasis", "Galaxy" };
		for (int i = 0; i < 5; i++) {
			bool is_active = (ui_actions.current_mode == (i + 1));
			if (is_active) ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "> %d: %s", i + 1, modes[i]);
			else ImGui::Text("  %d: %s", i + 1, modes[i]);
		}
		ImGui::Text("E/G: Reset particles in front of camera");
		ImGui::Separator();
		ImGui::Text("F: Launch Firework");
		ImGui::Separator();
		ImGui::Text("P: Toggle controls/performance ui");
		ImGui::Text("Tab: Toggle Settings");
	}
	ImGui::End();
}

} // namespace ve

// Called by the entry point to create the application instance
ve::VeApplication* createApp(std::filesystem::path project_root) {
	return new ve::Sandbox(project_root);
}

