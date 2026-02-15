#include <cmath>
#include <glm/glm.hpp>
#define GLM_FORCE_RADIANS
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>

#include "sandbox.hpp"
#include "utils/ve_random.hpp"
#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdlib>

namespace ve {

// First a window, device and swap chain are initialised in the base class
Sandbox::Sandbox(const std::filesystem::path& working_dir) : m_paths(working_dir) {
	m_resource_manager = std::make_unique<VeResourceManager>(m_ve_device);
	createBuffers();
	createDescriptors();

	initSystems();
	initUI();

	// Load initial scene
	loadScene(ui_actions.current_scene);

	m_camera.setPerspective(m_fov, m_last_aspect, m_near_plane, m_far_plane);
}

void Sandbox::loadScene(SandboxUIContext::SceneType scene_type) {
	if (scene_type == SandboxUIContext::SceneType::NONE)
		return;
	if (m_loaded_scene_type == scene_type)
		return;
	unloadScene();
	switch (scene_type) {
		case SandboxUIContext::SceneType::SIMPLE:
			VE_LOGD("Loading SimpleScene...");
			m_active_scene = std::make_unique<SimpleScene>(m_ve_device, *m_resource_manager, *m_global_pool, *m_material_set_layout, m_paths, &m_default_material_descriptor_set);
			break;
		case SandboxUIContext::SceneType::SPONZA:
			VE_LOGD("Loading SponzaScene (UASTC)...");
			m_active_scene = std::make_unique<SponzaScene>(m_ve_device, *m_resource_manager, *m_global_pool, *m_material_set_layout, m_paths, "sponza");
			break;
		case SandboxUIContext::SceneType::SPONZA_LOW:
			VE_LOGD("Loading SponzaScene (ETC1S)...");
			m_active_scene = std::make_unique<SponzaScene>(m_ve_device, *m_resource_manager, *m_global_pool, *m_material_set_layout, m_paths, "sponza_low");
			break;
		case SandboxUIContext::SceneType::BISTRO:
			VE_LOGD("Loading BistroScene...");
			m_active_scene = std::make_unique<BistroScene>(m_ve_device, *m_resource_manager, *m_global_pool, *m_material_set_layout, m_paths);
			break;
		default:
			return;
	}
	m_loaded_scene_type = scene_type;
	m_shadow_render_system->invalidateShadowDrawables();
	ui_actions.sun_intensity = m_active_scene->getSunIntensity();
	VE_LOGD("Scene loaded successfully.");
}

void Sandbox::unloadScene() {
	if (!m_active_scene)
		return;
	// Wait for GPU to finish all submitted work before freeing scene resources
	m_ve_device.getDevice().waitIdle();
	m_active_scene.reset();
	m_loaded_scene_type = SandboxUIContext::SceneType::NONE;
}

Sandbox::~Sandbox() {}

VeFrameInfo Sandbox::update() {
	m_cpu_start = std::chrono::steady_clock::now();

	assert(m_active_scene && "No scene loaded, this should not happen.");

	// Get frame time
	updateFrameTime();
	m_total_time += m_frame_time;

	if (ui_actions.apply_particle_count) {
		m_particle_system->applyStagedParticleCount();
		ui_actions.apply_particle_count = false;
	}

	// Setup frame info
	auto& command_buffer = m_ve_renderer.getCurrentCommandBuffer();
	auto& compute_command_buffer = m_ve_renderer.getCurrentComputeCommandBuffer();
	auto current_frame = m_ve_renderer.getCurrentFrame();

	// Dynamically load/unload scenes when selection changes (defer by one frame to show "Loading...")
	if (m_pending_scene_load != SandboxUIContext::SceneType::NONE) {
		loadScene(m_pending_scene_load);
		m_pending_scene_load = SandboxUIContext::SceneType::NONE;
	} else if (ui_actions.current_scene != m_loaded_scene_type) {
		m_pending_scene_load = ui_actions.current_scene;
	}

	vk::raii::DescriptorSet& material_descriptor_set = m_active_scene->getDescriptorSet();

	vk::raii::DescriptorSet& shadow_desc_set = m_shadow_render_system->getShadowDescriptorSet(current_frame);

	int color_space_type = 0; // default SRGB
	auto swapchain_color_space = m_ve_renderer.getSwapChainColorSpace();
	if (swapchain_color_space == vk::ColorSpaceKHR::eExtendedSrgbLinearEXT) {
		color_space_type = 1;
	} else if (swapchain_color_space == vk::ColorSpaceKHR::eHdr10St2084EXT) {
		color_space_type = 2;
	}

	auto extent = m_ve_renderer.getExtent();
	glm::vec2 texel_size = {1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};

	VeFrameInfo frame_info = {
		.global_descriptor_set = m_global_descriptor_sets[current_frame],
		.texture_descriptor_set = m_particle_descriptor_set,
		.material_descriptor_set = material_descriptor_set,
		.active_scene = m_active_scene.get(),
		.cubemap_descriptor_set = m_skybox_render_system->getCubemapDescriptorSet(), // currently only consumed by skybox render system itself.
		.shadow_descriptor_set = shadow_desc_set,
		.command_buffer = command_buffer,
		.compute_command_buffer = compute_command_buffer,
		.camera = m_camera,
		.registry = &m_active_scene->getRegistry(),
		.visible_objects = m_culling_system->getVisibleObjectsRef(),
		.frame_time = m_frame_time,
		.total_time = m_total_time,
		.current_frame = current_frame,
		.post_process_push = {
			ui_actions.blur_radius,
			ui_actions.blur_strength,
			ui_actions.exposure,
			color_space_type,
			ui_actions.bloom_enabled ? ui_actions.bloom_strength : 0.0f,
			{0.0f, 0.0f, 0.0f}, // padding
			texel_size
		},
		.instance_data = static_cast<InstanceData*>(m_instance_buffers[current_frame]->getMappedMemory()),
		.instance_count = 0,
		.instance_capacity = INITIAL_INSTANCE_CAPACITY,
		.compute_query_pool = m_ve_renderer.getQueryPool(),
		.compute_start_query = m_ve_renderer.getComputeStartQuery(),
	};

	// Updates camera state based on input and frame time. Returns actions for systems.
	auto input_actions = m_input_controller.processInput(m_frame_time, m_camera);

	// Update UI state
	// Tab toggles "UI Mode" (cursor visible). Settings window is ONLY visible in UI mode.
	ui_actions.visible = input_actions.ui_visible;

	// P key toggles performance and controls windows
	if (input_actions.toggle_performance_ui) {
		ui_actions.show_performance = !ui_actions.show_performance;
		ui_actions.show_controls = !ui_actions.show_controls;
	}

	updateCamera(glm::radians(ui_actions.fov));
	updateParticles(input_actions);

	// update scene
	m_active_scene->setSunIntensity(ui_actions.sun_intensity);
	m_active_scene->update(m_frame_time);

	// update ubos
	UniformBufferObject ubo{};
	ubo.render_mode = ui_actions.render_mode;
	ubo.shadow_mode = ui_actions.shadow_mode;
	ubo.ambient_light_color = glm::vec4(ui_actions.ambient_light_color, ui_actions.ambient_light_intensity);
	m_point_light_system->updateUniformBuffer(frame_info, ubo); // update UBO with point light data
	m_shadow_render_system->updateUniformBuffer(current_frame, ubo); // update internal shadow UBO with light data from main UBO
	this->updateUniformBuffer(current_frame, ubo); // view/proj/camera location in application base class

	// Record and submit compute work (two particle systems)
	m_fireworks_system->recordComputeCommands(frame_info);
	m_particle_system->recordComputeCommands(frame_info);

	m_ve_renderer.submitCompute(frame_info.compute_command_buffer);

	return frame_info;
}

// Update particle system based on input actions and UI context
// Consider moving this to the particle system class
void Sandbox::updateParticles(InputActions& actions) {
	// Apply input actions
	if (actions.set_mode >= 1 && actions.set_mode <= 5) {
		ParticleMode mode = static_cast<ParticleMode>(actions.set_mode);
		m_particle_system->setMode(mode);
		ui_actions.current_mode = mode;
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
	if (!m_active_scene) {
		VE_LOGD("No scene loaded, this should not happen.");
		return;
	}

	auto& command_buffer = frame_info.command_buffer;

	// Culling pass
	m_culling_system->setCullingEnabled(ui_actions.enable_frustum_culling);
	m_culling_system->cullObjects(frame_info);
	ui_actions.cull_total_objects = m_culling_system->getLastTotalMeshObjects();
	ui_actions.cull_visible_objects = m_culling_system->getLastVisibleCount();

	// Shadow pass: render a shadow map for each light
	if (ui_actions.shadow_mode != ShadowMode::DISABLED) {
		m_shadow_render_system->renderShadowMaps(frame_info);
	}

	// Main scene pass
	if (m_active_scene->getType() == VeScene::Type::SIMPLE) {
		m_ve_renderer.beginSceneRender(command_buffer);
		m_skybox_render_system->render(frame_info);
		m_simple_render_system->renderObjects(frame_info);
		m_particle_system->render(frame_info);
	} else {
		m_pbr_render_system->prepareFrame(frame_info);

		// Depth pre-pass: render opaque depth before color pass
		if (ui_actions.depth_prepass_enabled) {
			m_ve_renderer.beginDepthPrePass(command_buffer);
			m_depth_prepass_system->render(frame_info, m_pbr_render_system->getOpaqueGroups());
			m_ve_renderer.endDepthPrePass(command_buffer);
		}

		m_ve_renderer.beginSceneRender(command_buffer, ui_actions.depth_prepass_enabled);

		m_pbr_render_system->renderOpaque(frame_info);
		m_skybox_render_system->renderAsBackground(frame_info);
		m_pbr_render_system->renderTransparent(frame_info);
	}
	if (ui_actions.show_axes) {
		m_axes_render_system->render(frame_info);
	}
	if (ui_actions.show_aabb_debug) {
		m_aabb_debug_render_system->render(frame_info);
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

	// Record CPU time before UI rendering
	auto cpu_end = std::chrono::steady_clock::now();
	ui_actions.cpu_time = std::chrono::duration<float, std::chrono::milliseconds::period>(cpu_end - m_cpu_start).count();

	// Update GPU time from renderer
	ui_actions.gpu_time = m_ve_renderer.getGpuTime();
	ui_actions.compute_gpu_time = m_ve_renderer.getComputeGpuTime();
	ui_actions.gpu_overlap = m_ve_renderer.getGpuOverlap();

	// Draw UI: begin frame, render app-specific windows, render engine windows, end frame
	m_imgui_layer->renderUI(ui_actions, [this](UIContext&) {
		this->renderAppWindows();
		this->renderControlsWindow();
	});
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
	m_aabb_debug_render_system->recreatePipeline(offscreen_format, sample_count);
	m_axes_render_system->recreatePipeline(offscreen_format, sample_count);
	m_skybox_render_system->recreatePipeline(offscreen_format, sample_count);
	m_particle_system->recreatePipeline(offscreen_format, sample_count);
	m_fireworks_system->recreatePipeline(offscreen_format, sample_count);

	m_depth_prepass_system->recreatePipeline(sample_count);

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
				if (m_pending_scene_load != SandboxUIContext::SceneType::NONE) {
					ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Loading...");
				}
				// Select exactly 1 scene
				int current_scene_int = static_cast<int>(ui_actions.current_scene);
				if (ImGui::RadioButton("Simple", &current_scene_int, static_cast<int>(SandboxUIContext::SceneType::SIMPLE))) {
					ui_actions.current_scene = SandboxUIContext::SceneType::SIMPLE;
				}
				if (ImGui::RadioButton("Sponza", &current_scene_int, static_cast<int>(SandboxUIContext::SceneType::SPONZA))) {
					ui_actions.current_scene = SandboxUIContext::SceneType::SPONZA;
				}
				if (ImGui::RadioButton("Sponza (compressed textures)", &current_scene_int, static_cast<int>(SandboxUIContext::SceneType::SPONZA_LOW))) {
					ui_actions.current_scene = SandboxUIContext::SceneType::SPONZA_LOW;
				}
				if (ImGui::RadioButton("Bistro", &current_scene_int, static_cast<int>(SandboxUIContext::SceneType::BISTRO))) {
					ui_actions.current_scene = SandboxUIContext::SceneType::BISTRO;
				}
				ImGui::Separator();
				ImGui::Text("Scene Settings");
				ImGui::SliderFloat("Sun Intensity", &ui_actions.sun_intensity, 0.0f, 600000.0f);

				ImGui::Separator();
				if (ImGui::CollapsingHeader("Point Lights")) {
					if (m_active_scene) {
						auto& registry = m_active_scene->getRegistry();
						auto& pl_pool = registry.pointLights();

						// Collect light entities sorted by id for stable UI order
						std::vector<Entity> lights;
						lights.reserve(pl_pool.size());
						for (uint32_t i = 0; i < pl_pool.size(); i++) {
							Entity e = registry.entityFromIndex(pl_pool.entityAt(i));
							if (registry.hasComponent<TransformComponent>(e))
								lights.push_back(e);
						}
						std::sort(lights.begin(), lights.end());

						if (!lights.empty()) {
							if (ImGui::Button("All on")) {
								for (auto e : lights) registry.setActive(e, true);
							}
							ImGui::SameLine();
							if (ImGui::Button("All off")) {
								for (auto e : lights) registry.setActive(e, false);
							}
						}
						const Entity scene_sun = m_active_scene->getSun();
						for (size_t i = 0; i < lights.size(); i++) {
							Entity e = lights[i];
							auto* pl = registry.getComponent<PointLightComponent>(e);
							auto* transform = registry.getComponent<TransformComponent>(e);
							if (!pl || !transform)
								continue;
							ImGui::PushID(static_cast<int>(e.id()));
							const auto& name = registry.getName(e);
							const std::string label = name.empty() ? ("Light " + std::to_string(static_cast<unsigned>(i))) : name;
							if (ImGui::TreeNode(label.c_str())) {
								bool active = registry.isActive(e);
								ImGui::Checkbox("Active", &active);
								registry.setActive(e, active);
								ImGui::ColorEdit3("Color", &pl->color.r);
								ImGui::SliderFloat("Intensity", &pl->intensity, 0.0f, 100000.0f);
								if (!scene_sun.isNull() && e == scene_sun)
									ui_actions.sun_intensity = pl->intensity;
								glm::vec3 pos = transform->getTranslation();
								if (ImGui::DragFloat3("Position", &pos.x)) {
									transform->setTranslation(pos);
								}

								static float size = transform->getScale().x;
								if (ImGui::SliderFloat("Size", &size, 0.1f, 50.0f)) {
									transform->setScale({size, size, size});
								}
								ImGui::Checkbox("Rotates", &pl->rotates);
								ImGui::Checkbox("Casts shadow", &pl->casts_shadow);
								ImGui::TreePop();
							}
							ImGui::PopID();
						}
					}
				}

				ImGui::Separator();
				ImGui::Text("Skybox");
				ImGui::Separator();
				auto& skybox = *m_skybox_render_system;
				auto& skybox_settings = skybox.getSettings();
				if (skybox.isLoading()) {
					ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Loading...");
				}
				const auto& available = skybox.getAvailableSkyboxes();
				if (!available.empty()) {
					int current_idx = static_cast<int>(skybox.getCurrentSkyboxIndex());
					for (size_t i = 0; i < available.size(); i++) {
						if (ImGui::RadioButton(available[i].display_name.c_str(), &current_idx, static_cast<int>(i))) {
							skybox.setSkybox(i);
						}
					}
				} else {
					ImGui::TextDisabled("No skybox textures found (.ktx, .ktx2)");
				}
				ImGui::Checkbox("Rotate", &skybox_settings.rotate);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Disable for skyboxes with a horizon (e.g. clouds).");
				}
				ImGui::SliderFloat("Exposure", &skybox_settings.exposure, 0.1f, 5.0f, "%.2f");
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Skybox brightness (independent of post-process exposure).");
				}
				ImGui::Checkbox("Day", &skybox_settings.is_day);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Day: warm tint, Night: cool tint.");
				}

				ImGui::Separator();
				ImGui::Text("Ambient Light");
				ImGui::Separator();
				ImGui::ColorEdit3("Color", &ui_actions.ambient_light_color.r);
				ImGui::SliderFloat("Intensity", &ui_actions.ambient_light_intensity, 0.0f, 0.5f, "%.4f");
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Global ambient light intensity applied to all surfaces.");
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
				ImGui::SliderFloat("Particle Size", &config.explosion_size, 0.1f, 5.0f);
				ImGui::SliderFloat("Trail Interval", &config.trail_interval, 0.0001f, 0.1f, "%.4f");
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Interval at which to emit smoke/trail particles.");
				}

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
			if (ImGui::BeginTabItem("Display")) {
				ImGui::Text("Display & Camera Settings");
				ImGui::Separator();

				// FOV slider
				ImGui::Text("Field of View");
				if (ImGui::SliderFloat("Degrees", &ui_actions.fov, 30.0f, 120.0f, "%.1f")) {
					// Handled in update()
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Field of View in degrees.");
				}
				ImGui::Separator();

				// VSync toggle (eMailbox instead of eImmediate)
				if (ImGui::Checkbox("Enable VSync", &ui_actions.vsync)) {
					m_ve_renderer.setPresentMode(ui_actions.vsync ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate);
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Synchronizes frame rate with display refresh rate.\nEliminates screen tearing but may increase input latency.");
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
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("High Dynamic Range.\nRequires compatible HDR display.");
				}

				ImGui::Separator();
				auto extent = m_ve_renderer.getExtent();
				ImGui::Text("Resolution: %d x %d", extent.width, extent.height);

				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Graphics")) {
				ImGui::Text("Graphics Quality Settings");
				ImGui::Separator();

				// MSAA slider with discrete sample counts
				{
					ImGui::Text("MSAA:");

					// Get device max and current sample count
					vk::SampleCountFlagBits max_samples = m_ve_renderer.getMaxSampleCount();
					vk::SampleCountFlagBits current_samples = m_ve_renderer.getSampleCount();

					// Build list of available sample counts (powers of 2 up to device max)
					std::vector<vk::SampleCountFlagBits> available_samples;
					std::vector<std::string> sample_labels;
					available_samples.push_back(vk::SampleCountFlagBits::e1);
					sample_labels.push_back("Off");

					for (int i = 2; i <= 64; i *= 2) {
						vk::SampleCountFlagBits sample_flag = static_cast<vk::SampleCountFlagBits>(i);
						if (sample_flag <= max_samples) {
							available_samples.push_back(sample_flag);
							sample_labels.push_back(std::to_string(i) + "x");
						}
					}

					// Find current index
					size_t current_index = 0;
					for (size_t i = 0; i < available_samples.size(); i++) {
						if (available_samples[i] == current_samples) {
							current_index = i;
							break;
						}
					}

					// Slider
					int slider_value = static_cast<int>(current_index);
					ImGui::PushItemWidth(200.0f);
					if (ImGui::SliderInt("##msaa_slider", &slider_value, 0, static_cast<int>(available_samples.size()) - 1, "")) {
						slider_value = std::clamp(slider_value, 0, static_cast<int>(available_samples.size()) - 1);
						m_ve_renderer.setSampleCount(available_samples[static_cast<size_t>(slider_value)]);
					}
					ImGui::PopItemWidth();
					ImGui::SameLine();
					ImGui::Text("%s", sample_labels[current_index].c_str());
					if (ImGui::IsItemHovered()) {
						ImGui::BeginTooltip();
						ImGui::Text("Multi-Sample Anti-Aliasing");
						ImGui::Separator();
						ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Smooths jagged edges on geometry.");
						ImGui::Text("Off (1x): No MSAA, best performance");
						ImGui::Text("2x-4x: Balanced quality/performance");
						ImGui::Text("8x+: High quality, significant cost");
						ImGui::EndTooltip();
					}
				}

				// Shadow mode selection - slider with discrete positions
				ImGui::Text("Shadows:");
				ImGui::PushItemWidth(200.0f);
				int shadow_mode_int = static_cast<int>(ui_actions.shadow_mode);
				if (ImGui::SliderInt("##shadow_slider", &shadow_mode_int, 0, 2, "")) {
					// Ensure it snaps to discrete values
					ui_actions.shadow_mode = static_cast<ShadowMode>(std::clamp(shadow_mode_int, 0, 2));
				}
				ImGui::PopItemWidth();
				ImGui::SameLine();
				const char* shadow_labels[] = { "Off", "Normal", "PCF" };
				ImGui::Text("%s", shadow_labels[static_cast<uint32_t>(ui_actions.shadow_mode)]);
				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					ImGui::Text("Shadow Rendering Mode");
					ImGui::Separator();
					ImGui::Text("Off: No shadows, best performance");
					ImGui::Text("Normal: Hard shadows, good performance");
					ImGui::Text("PCF: Percentage Closer Filtering");
					ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "     Soft shadow edges, higher cost");
					ImGui::EndTooltip();
				}

				// Topology selection. Requires pipeline recreation
				ImGui::Text("Topology: ");
				ImGui::SameLine();
				int topology_int = static_cast<int>(ui_actions.topology);
				if (ImGui::RadioButton("Triangle List", &topology_int, static_cast<int>(Topology::TRIANGLE_LIST))) {
					ui_actions.topology = Topology::TRIANGLE_LIST;
					m_simple_render_system->setTopology(vk::PrimitiveTopology::eTriangleList);
					m_pbr_render_system->setTopology(vk::PrimitiveTopology::eTriangleList);
					m_ve_renderer.setSwapChainNeedsRecreation();
				}
				ImGui::SameLine();
				if (ImGui::RadioButton("Line List", &topology_int, static_cast<int>(Topology::LINE_LIST))) {
					ui_actions.topology = Topology::LINE_LIST;
					m_simple_render_system->setTopology(vk::PrimitiveTopology::eLineList);
					m_pbr_render_system->setTopology(vk::PrimitiveTopology::eLineList);
					m_ve_renderer.setSwapChainNeedsRecreation();
				}


				ImGui::Separator();
				ImGui::Text("Render mode (sponza): ");
				int current_render_mode = static_cast<int>(ui_actions.render_mode);
				if (ImGui::RadioButton("BRDF Microfacets", &current_render_mode, static_cast<int>(RenderMode::BRDF_MICROFACET)))
					ui_actions.render_mode = RenderMode::BRDF_MICROFACET;
				ImGui::SameLine();
				if (ImGui::RadioButton("BRDF Smooth", &current_render_mode, static_cast<int>(RenderMode::BRDF)))
					ui_actions.render_mode = RenderMode::BRDF;
				if (ImGui::RadioButton("Normal vector", &current_render_mode, static_cast<int>(RenderMode::NORMAL_VECTOR)))
					ui_actions.render_mode = RenderMode::NORMAL_VECTOR;
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Visualize surface normals (RGB = XYZ)");
				}
				if (ImGui::RadioButton("Tangent vector", &current_render_mode, static_cast<int>(RenderMode::TANGENT_VECTOR)))
					ui_actions.render_mode = RenderMode::TANGENT_VECTOR;
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Visualize tangent vectors for normal mapping");
				}
				if (ImGui::RadioButton("Bitangent vector", &current_render_mode, static_cast<int>(RenderMode::BITANGENT_VECTOR)))
					ui_actions.render_mode = RenderMode::BITANGENT_VECTOR;
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Visualize bitangent vectors for normal mapping");
				}
				if (ImGui::RadioButton("Normal map", &current_render_mode, static_cast<int>(RenderMode::NORMAL_MAP)))
					ui_actions.render_mode = RenderMode::NORMAL_MAP;
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Display normal map texture data directly");
				}

				// Axes system visibility
				ImGui::Separator();
				ImGui::Checkbox("Show Axes", &ui_actions.show_axes);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Display XYZ coordinate axes in the scene.\nRed=X, Green=Y, Blue=Z");
				}
				ImGui::Checkbox("Show AABB outlines", &ui_actions.show_aabb_debug);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Display wireframe bounding boxes for visible objects");
				}
				ImGui::Checkbox("Frustum culling", &ui_actions.enable_frustum_culling);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Skip drawing objects outside the camera view");
				}
				ImGui::Checkbox("Depth Pre-Pass", &ui_actions.depth_prepass_enabled);

				ImGui::Separator();
				ImGui::Text("Post Processing: ");
				ImGui::SliderInt("Blur Radius", &ui_actions.blur_radius, 0, 10);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Gaussian blur kernel size.\n0 = No blur, higher values = stronger blur effect");
				}
				ImGui::SliderFloat("Blur Strength", &ui_actions.blur_strength, 0.0f, 5.0f);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Intensity of the blur effect");
				}
				ImGui::SliderFloat("Exposure", &ui_actions.exposure, 0.0f, 5.0f);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Tone mapping exposure adjustment.\n< 1.0: Darker, > 1.0: Brighter");
				}

				ImGui::Text("Bloom");
				ImGui::Separator();
				ImGui::Checkbox("Bloom Enabled", &ui_actions.bloom_enabled);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Bloom effect creates glow around bright areas.\nSimulates light bleeding in cameras");
				}
				ImGui::DragFloat("Bloom Strength", &ui_actions.bloom_strength, 0.001f, 0.0f, 0.4f);

				ImGui::EndTabItem();
			}

		}
		ImGui::EndTabBar();
	}
	ImGui::End();
}

void Sandbox::createBuffers() {
	VE_LOGD("Creating buffers");
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

	// Per-frame instance SSBO buffers (for instanced draw transforms)
	m_instance_buffers.clear();
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_instance_buffers.emplace_back(std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(InstanceData),
			INITIAL_INSTANCE_CAPACITY,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		));
		m_instance_buffers[i]->map();
	}
}

// Creates descriptor set layouts, pool, and descriptor sets used by render systems.
// - Global sets (per frame): UBO for view/proj, lights, etc.
// - Material layout: 5 texture bindings (albedo, normal, metallic-roughness, occlusion, emissive) + 1 UBO.
// - Particle descriptor set: glow/fire/smoke textures for particle system and point lights.
// - Default material descriptor set: untextured fallback (default albedo/normal/MR/occlusion/emissive + UBO)
void Sandbox::createDescriptors() {
	VE_LOGD("Creating descriptors");

	// Global set layout: UBO (binding 0), instance SSBO (binding 1), optional ray tracing (binding 2)
	m_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
#if ENABLE_RAY_TRACING
		.addBinding(2, vk::DescriptorType::eAccelerationStructureKHR, vk::ShaderStageFlagBits::eFragment)
#endif
		.build();

	// Material set layout: albedo, normal, metallic-roughness, occlusion, emissive
	m_material_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(2, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(3, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(4, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(5, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eFragment)
		.build();

	// Create descriptor pool
	// Material sets: SimpleScene ~3, Sponza ~170, Bistro 500+; allocate 4096 to avoid fragmentation / OUT_OF_POOL_MEMORY
	constexpr uint32_t MAX_MATERIAL_SETS = 4096;
	m_global_pool = VeDescriptorPool::Builder(m_ve_device)
		// Global sets + shadow global + compute + material sets + shadow + slack
		.setMaxSets(4 * MAX_FRAMES_IN_FLIGHT + (MAX_LIGHTS * MAX_FRAMES_IN_FLIGHT) + 10 + MAX_FRAMES_IN_FLIGHT + MAX_MATERIAL_SETS)
		// Uniform buffers: global (per frame) + shadow global (per frame, per light) + compute (2x per frame)
		.addPoolSize(vk::DescriptorType::eUniformBuffer, 4 * MAX_FRAMES_IN_FLIGHT + (MAX_LIGHTS * MAX_FRAMES_IN_FLIGHT) + MAX_MATERIAL_SETS + 1)
		// Combined image samplers: SimpleScene (~3) + per-material (5 per set) + skybox (3) + slack
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, 3 * 3 + MAX_MATERIAL_SETS * 5)
		// Samplers: shadow sampler (1 per frame) + slack
		.addPoolSize(vk::DescriptorType::eSampler, MAX_FRAMES_IN_FLIGHT + 7)
		// Sampled images: shadow map array (1 per frame) + slack
		.addPoolSize(vk::DescriptorType::eSampledImage, MAX_FRAMES_IN_FLIGHT + 7)
		// Storage buffers: compute + instance SSBO (1 per frame for global + 1 per frame per shadow light)
		.addPoolSize(vk::DescriptorType::eStorageBuffer, 14 * MAX_FRAMES_IN_FLIGHT + MAX_FRAMES_IN_FLIGHT + MAX_SHADOW_LIGHTS * MAX_FRAMES_IN_FLIGHT)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
		.buildShared();

	// Create global descriptor sets (per frame): UBO (binding 0) + instance SSBO (binding 1)
	m_global_descriptor_sets.clear();
	m_global_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto buffer_info = m_uniform_buffers[i]->getDescriptorInfo();
		auto instance_info = m_instance_buffers[i]->getDescriptorInfo();
		vk::raii::DescriptorSet set{nullptr};
		VeDescriptorWriter(*m_global_set_layout, *m_global_pool)
			.writeBuffer(0, &buffer_info)
			.writeBuffer(1, &instance_info)
			.build(set);
		m_global_descriptor_sets.push_back(std::move(set));
	}

	// Cubemap descriptor set is owned by SkyboxRenderSystem (created in initSystems)

	// Default material UBO for untextured meshes (binding 5).
	// Must match shader's MaterialConstants layout (4 × float4 = MATERIAL_UBO_SIZE bytes).
	m_default_material_ubo = std::make_unique<VeBuffer>(m_ve_device, MATERIAL_UBO_SIZE, 1,
		vk::BufferUsageFlagBits::eUniformBuffer,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	MaterialFactors defaults{};
	float default_ubo[16];
	writeMaterialUBO(default_ubo, defaults);
	m_default_material_ubo->map();
	m_default_material_ubo->writeToBuffer(default_ubo, MATERIAL_UBO_SIZE);
	m_default_material_ubo->unmap();
	auto default_material_ubo_info = m_default_material_ubo->getDescriptorInfo();

	// Particle descriptor set: glow/fire/smoke + default occlusion/emissive for layout, UBO at 5. Used by particle system and point lights.
	m_particle_texture_handle = m_resource_manager->load<VeTexture>(m_paths.particle_texture.lexically_normal().generic_string());
	m_fire_texture_handle = m_resource_manager->load<VeTexture>(m_paths.fire_texture.lexically_normal().generic_string());
	m_smoke_texture_handle = m_resource_manager->load<VeTexture>(m_paths.smoke_texture.lexically_normal().generic_string());
	m_default_occlusion_handle = m_resource_manager->load<VeTexture>("default_occlusion");
	m_default_emissive_handle = m_resource_manager->load<VeTexture>("default_emissive");
	auto particle_glow_info = m_particle_texture_handle.get()->getDescriptorInfo();
	auto particle_fire_info = m_fire_texture_handle.get()->getDescriptorInfo();
	auto particle_smoke_info = m_smoke_texture_handle.get()->getDescriptorInfo();
	auto particle_occlusion_info = m_default_occlusion_handle.get()->getDescriptorInfo();
	auto particle_emissive_info = m_default_emissive_handle.get()->getDescriptorInfo();
	m_particle_descriptor_set = vk::raii::DescriptorSet{nullptr};
	VeDescriptorWriter(*m_material_set_layout, *m_global_pool)
		.writeImage(0, &particle_glow_info)
		.writeImage(1, &particle_fire_info)
		.writeImage(2, &particle_smoke_info)
		.writeImage(3, &particle_occlusion_info)
		.writeImage(4, &particle_emissive_info)
		.writeBuffer(5, &default_material_ubo_info)
		.build(m_particle_descriptor_set);

	// Default material descriptor set: untextured fallback for Simple scene (floor, vases). Full material layout with defaults.
	// Must keep texture handles as members so they outlive the descriptor set (descriptors reference VkImageView/VkSampler).
	m_default_albedo_handle = m_resource_manager->load<VeTexture>("default_albedo");
	m_default_normal_handle = m_resource_manager->load<VeTexture>("default_normal");
	m_default_mr_handle = m_resource_manager->load<VeTexture>("default_metallic_roughness");
	auto default_albedo_info = m_default_albedo_handle.get()->getDescriptorInfo();
	auto default_normal_info = m_default_normal_handle.get()->getDescriptorInfo();
	auto default_mr_info = m_default_mr_handle.get()->getDescriptorInfo();
	m_default_material_descriptor_set = vk::raii::DescriptorSet{nullptr};
	VeDescriptorWriter(*m_material_set_layout, *m_global_pool)
		.writeImage(0, &default_albedo_info)
		.writeImage(1, &default_normal_info)
		.writeImage(2, &default_mr_info)
		.writeImage(3, &particle_occlusion_info)
		.writeImage(4, &particle_emissive_info)
		.writeBuffer(5, &default_material_ubo_info)
		.build(m_default_material_descriptor_set);
}

void Sandbox::initSystems() {
	VE_LOGD("Initialising systems");

	m_culling_system = std::make_unique<CullingSystem>(m_camera);

	// Create shadow system first (before other render systems that might need shadow descriptor set)
	VE_LOGD("shadow system: " << m_paths.shader("shadow_shader.spv"));
	m_shadow_render_system = std::make_unique<ShadowRenderSystem>(
		m_ve_device,
		*m_global_pool,
		m_material_set_layout->getDescriptorSetLayout(),
		m_instance_buffers,
		m_paths.shader("shadow_shader.spv")
	);

	m_depth_prepass_system = std::make_unique<DepthPrePassSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSampleCount(),
		m_paths.shader("shadow_shader.spv")
	);

	VE_LOGD("simple system: " << m_paths.shader("simple_shader.spv"));
	m_simple_render_system = std::make_unique<SimpleRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount(),
		m_paths.shader("simple_shader.spv")
	);
	VE_LOGD("pbr system: " << m_paths.shader("pbr_shader.spv"));
	m_pbr_render_system = std::make_unique<PbrRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount(),
		m_paths.shader("pbr_shader.spv")
	);
	VE_LOGD("aabb debug system: " << m_paths.shader("axes_shader.spv"));
	m_aabb_debug_render_system = std::make_unique<AabbDebugRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount(),
		m_paths.shader("axes_shader.spv")
	);
	VE_LOGD("axes system: " << m_paths.shader("axes_shader.spv"));
	m_axes_render_system = std::make_unique<AxesRenderSystem>(
		m_ve_device,
		*m_resource_manager,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount(),
		m_paths.shader("axes_shader.spv")
	);
	VE_LOGD("pl system: " << m_paths.shader("point_light_shader.spv"));
	m_point_light_system = std::make_unique<PointLightSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount(),
		m_paths.shader("point_light_shader.spv")
	);
	VE_LOGD("particle system: " << m_paths.shader("particle_compute.spv"));
	m_particle_system = std::make_unique<ParticleSystem>(
		m_ve_device,
		m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount(),
		50000, // number of particles
		glm::vec3{0.0f, -300.0f, 50.0f},
		m_paths.shader("particle_compute.spv")
	);
	m_fireworks_system = std::make_unique<FireworksSystem>(
		m_ve_device,
		m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount(),
		m_paths.shader("particle_compute.spv")
	);
	VE_LOGD("skybox system: " << m_paths.shader("skybox_shader.spv"));
	m_skybox_render_system = std::make_unique<SkyboxRenderSystem>(
		m_ve_device,
		*m_resource_manager,
		*m_global_pool,
		*m_material_set_layout,
		m_global_set_layout->getDescriptorSetLayout(),
		m_paths.skybox_dir,
		m_paths.shader("skybox_shader.spv"),
		m_paths.cube_model,
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount()
	);

	m_bloom_system = std::make_unique<BloomSystem>(
		m_ve_device,
		m_ve_renderer.getExtent(),
		m_ve_renderer.getResolveTargetImageView(),
		m_paths.shader("bloom_downsample.spv"),
		m_paths.shader("bloom_upsample.spv")
	);

	m_post_process_system = std::make_unique<PostProcessSystem>(
		m_ve_device,
		m_ve_renderer.getSwapChainImageFormat(),
		m_ve_renderer.getResolveTargetImageView(),
		m_bloom_system->getBloomTexture(),
		m_paths.shader("post_process.spv")
	);
}

void Sandbox::initUI() {
	VE_LOGD("Initialising UI");
	m_imgui_layer = std::make_unique<ImGuiLayer>(m_ve_window, m_ve_device, m_ve_renderer);

	ui_actions.hdr_enabled = m_ve_renderer.hasHdrSupport() && m_ve_renderer.isHdrEnabled();
	ui_actions.fov = glm::degrees(m_fov);
	ui_actions.current_mode = m_particle_system->getMode();
	ui_actions.speed = m_particle_system->getSpeed();
	ui_actions.pending_particle_count = m_particle_system->getPendingParticleCount();
	ui_actions.particle_velocity_mean = m_particle_system->getMean();
	ui_actions.particle_velocity_stddev = m_particle_system->getStddev();
	ui_actions.min_life = m_particle_system->getMinLife();
	ui_actions.max_life = m_particle_system->getMaxLife();
	ui_actions.should_respawn = m_particle_system->getShouldRespawn();
	ui_actions.ambient_light_color = glm::vec3(DEFAULT_AMBIENT_LIGHT_COLOR);
	ui_actions.ambient_light_intensity = DEFAULT_AMBIENT_LIGHT_COLOR.w;
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
		// Modes correspond to the ParticleMode enum in particle_system.hpp
		const char* modes[] = { "Earth Gravity", "Cool Gravity", "Succ mode", "Stasis", "Galaxy" };
		for (int i = 0; i < 5; i++) {
			ParticleMode mode = static_cast<ParticleMode>(i + 1);
			bool is_active = (ui_actions.current_mode == mode);
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

