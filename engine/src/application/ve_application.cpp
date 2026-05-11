#include "application/ve_application.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_device.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/editor.hpp"
#include "vulkan/ve_buffer.hpp"
#include "input/input_controller.hpp"
#include "input/input_action.hpp"
#include "scene/ve_camera.hpp"
#include "scene/ve_scene.hpp"
#include "scene/gltf_scene.hpp"
#include "utils/ve_log.hpp"

// Render systems
#include "rendering/culling/culling_system.hpp"
#include "rendering/shadow_render_system.hpp"
#include "rendering/depth_prepass_system.hpp"
#include "rendering/shadow_mask_system.hpp"
#include "rendering/gtao_system.hpp"
#include "rendering/cluster_light_system.hpp"
#include "rendering/pbr_render_system.hpp"
#include "rendering/aabb_debug_render_system.hpp"
#include "rendering/axes_render_system.hpp"
#include "rendering/light_system.hpp"
#include "rendering/particle_system.hpp"
#include "rendering/fireworks_system.hpp"
#include "rendering/skybox_render_system.hpp"
#include "rendering/ibl_system.hpp"
#include "rendering/bloom_system.hpp"
#include "rendering/post_process_system.hpp"
#include "rendering/outline_system.hpp"
#include "rendering/managers/scene_resource_manager.hpp"
#include "rendering/managers/bindless_texture_registry.hpp"
#include "rendering/managers/material_ssbo_manager.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "rendering/culling/gpu_culling_system.hpp"
#include "rendering/culling/meshlet_culling_system.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/skinning_pre_pass.hpp"
#include "rendering/skinned_points_render_system.hpp"
#include "rendering/culling/cpu_culling_backend.hpp"
#include "rendering/culling/gpu_culling_backend.hpp"
#include "rendering/culling/meshlet_culling_backend.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "ve_tracy.hpp"
#include "vulkan/ve_debug_utils.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <format>
#include <cassert>

namespace ve {

static vk::Extent2D halveExtent(vk::Extent2D e, bool half) {
	if (!half)
		return e;
	return {std::max(1u, e.width / 2), std::max(1u, e.height / 2)};
}

VeApplication::VeApplication(const EngineConfig& config)
	: m_ve_window(WIDTH, HEIGHT, APP_NAME),
	  m_ve_device(m_ve_window),
	  m_ve_renderer(m_ve_device, m_ve_window),
	  m_camera(glm::vec3{20.0f, 20.0f, 20.0f}, glm::vec3{0.0f, 0.0f, 1.0f}),
	  m_input_controller(m_ve_window),
	  m_config(config) {

	m_event_bus = std::make_unique<EventBus>();
	m_resource_manager = std::make_unique<VeResourceManager>(m_ve_device);
	m_asset_loader = std::make_unique<AssetLoadingSystem>(*m_resource_manager);
	createBuffers();
	m_scene_resources = std::make_unique<SceneResourceManager>(m_ve_device, *m_event_bus);
	createDescriptors();
	initSystems();
	initEditor();

	m_camera.setPerspective(m_fov, m_last_aspect, m_near_plane, m_far_plane);
}

VeApplication::~VeApplication() {
	m_ve_device.getDevice().waitIdle();
	m_active_scene.reset();
}

// ─── Main Loop ───────────────────────────────────────────────────────────────

void VeApplication::run() {
	VE_LOGI("VeApplication::run starting. Window=" + std::to_string(m_ve_window.getWidth()) + "x" + std::to_string(m_ve_window.getHeight()));

	setWindowTitle();

	while (!m_ve_window.shouldClose()) {
		m_ve_window.pollEvents();

		// Check for window resize or out-of-date swap chain BEFORE starting frame
		if (m_ve_window.wasWindowResized() || m_ve_renderer.isSwapChainOutOfDate()) {
			VE_LOGD("Swap chain recreation triggered in main loop.");
			m_ve_device.getDevice().waitIdle();
			m_ve_window.resetWindowResizedFlag();
			m_ve_renderer.recreateSwapChain();
			onSwapChainRecreated();
			continue;
		}

		m_ve_renderer.getProfiler().beginCpuTimer(ProfileTimer::FRAME_TOTAL);
		ZoneScopedN("Frame");

		{
			ZoneScopedN("Begin Frame");
			if (!m_ve_renderer.beginFrame())
				continue;
		}

		updateFrameTime();
		m_total_time += m_frame_time;

		// Process input and camera
		m_input_controller.processInput(m_frame_time, m_camera);
		m_ui.visible = m_input_controller.isEditorMode();
		m_editor->getState().editor_mode = m_input_controller.isEditorMode();
		updateCamera(glm::radians(m_ui.fov));

		// Process pending entity deletions at a safe point
		if (m_active_scene) {
			auto& registry = m_active_scene->getRegistry();
			if (registry.hasPendingDeletions()) {
				m_ve_device.getDevice().waitIdle();
				registry.processPendingDeletions();
			}
		}

		processSceneLoadRequest();
		tickAsyncLoader();

		m_event_bus->flushEvents();

		// App per-frame logic (particle config, etc.)
		update();

		if (!m_active_scene) {
			m_ve_renderer.beginUIRecording(m_editor->isEditorMode());
			m_editor->renderUI(m_ui, nullptr, nullptr);
			m_ve_renderer.endFrame();
			continue;
		}

		// Update scene
		{
			ZoneScopedN("Scene Update");
			m_active_scene->update(m_frame_time);
		}

		// Physics
		if (m_ui.physics_enabled) {
			ZoneScopedN("Physics Update");
			m_ve_renderer.getProfiler().beginCpuTimer(ProfileTimer::PHYSICS);
			m_physics_system->update(m_frame_time, m_active_scene->getRegistry());
			m_ve_renderer.getProfiler().endCpuTimer(ProfileTimer::PHYSICS);
		}

		// Flush collision events
		m_event_bus->flushEvents();

		// Engine pipeline
		pushPerFrameSettings();
		emitSettingEvents();
		if (m_editor->beginFrame())
			recreateResolutionDependentSystems(); // extent changed
		selectBackend();
		bool gpu_culling = m_ui.gpu_culling_enabled
			&& m_scene_resources->getGpuSceneManager().hasRegisteredObjects();
		bool hiz_on = m_ui.hiz_occlusion_enabled && m_ui.depth_prepass_enabled && gpu_culling;
		m_active_backend->setHizEnabled(hiz_on);
		m_skybox_render_system->processPendingLoad();

		VeFrameInfo fi = buildFrameInfo();
		{
			ZoneScopedN("Populate UBO");
			populateUBO(fi);
		}
		if (m_active_scene) {
			ZoneScopedN("Skinning Palette");
			m_skinning_pre_pass->updatePalette(m_active_scene->getRegistry(), fi.current_frame);
		}
		dispatchCompute(fi);
		renderFrame(fi);

		// UI on separate command buffer
		bool editor_mode = m_editor->isEditorMode();
		{
			ZoneScopedN("UI");
			m_ve_renderer.beginUIRecording(editor_mode);
			Registry* ui_registry = &m_active_scene->getRegistry();
			m_editor->renderUI(m_ui, ui_registry, m_active_scene.get());
		}

		collectStats(fi);
		{
			ZoneScopedN("End Frame");
			m_ve_renderer.endFrame();
		}
	}

	m_ve_device.getDevice().waitIdle();
}

const std::string& VeApplication::getAppSettingsWindowName() const {
	return m_imgui_layer->getAppSettingsWindowName();
}

// ─── Scene Management ────────────────────────────────────────────────────────

void VeApplication::setActiveScene(std::unique_ptr<VeScene> scene) {
	unloadScene();
	m_active_scene = std::move(scene);
	if (m_active_scene) {
		glm::vec4 ambient = m_active_scene->getDefaultAmbient();
		m_ui.ambient_light_color = glm::vec3(ambient);
		m_ui.ambient_light_intensity = ambient.w;

		m_event_bus->emitImmediate(SceneLoadedEvent{&m_active_scene->getRegistry()});
	}
}

Registry* VeApplication::getActiveRegistry() {
	return m_active_scene ? &m_active_scene->getRegistry() : nullptr;
}

void VeApplication::unloadScene() {
	if (!m_active_scene)
		return;
	m_event_bus->emitImmediate(SceneUnloadedEvent{});
	m_active_scene.reset();
}

void VeApplication::registerScene(const std::string& name,
								   std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory) {
	m_scene_entries.push_back({name, std::move(factory), {}, {}});
	if (m_current_scene_index < 0)
		m_current_scene_index = 0;
}

void VeApplication::registerAsyncScene(const std::string& name,
                                       const std::filesystem::path& gltf_path,
                                       std::function<std::unique_ptr<VeScene>(const SceneContext&, std::unique_ptr<VeModel>)> factory,
                                       bool extract_lights, bool flip_tex_coord_v) {
	m_scene_entries.push_back({name, {}, gltf_path, std::move(factory), extract_lights, flip_tex_coord_v});
	if (m_current_scene_index < 0)
		m_current_scene_index = 0;
}

void VeApplication::loadDefaultScene(int index) {
	if (index < 0 || index >= static_cast<int>(m_scene_entries.size()))
		return;
	auto ctx = getSceneContext();
	auto scene = m_scene_entries[static_cast<size_t>(index)].factory(ctx);
	setActiveScene(std::move(scene));
	m_loaded_scene_index = index;
	m_current_scene_index = index;
}

void VeApplication::processSceneLoadRequest() {
	if (m_pending_load.type == SceneLoadRequest::Type::NONE)
		return;

	auto ctx = getSceneContext();
	switch (m_pending_load.type) {
		case SceneLoadRequest::Type::LOAD_REGISTERED: {
			int idx = m_pending_load.scene_index;
			if (idx >= 0 && idx < static_cast<int>(m_scene_entries.size()) && idx != m_loaded_scene_index) {
				const auto& entry = m_scene_entries[static_cast<size_t>(idx)];
				if (!entry.gltf_path.empty() && entry.async_factory) {
					m_asset_loader->beginModelLoad(entry.gltf_path, entry.extract_lights, entry.flip_tex_coord_v);
					m_async_load_type = SceneLoadRequest::Type::LOAD_REGISTERED;
					m_pending_async_scene_index = idx;
				} else {
					m_asset_loader->cancel();
					auto scene = entry.factory(ctx);
					setActiveScene(std::move(scene));
					m_loaded_scene_index = idx;
					m_current_scene_index = idx;
				}
			}
			break;
		}
		case SceneLoadRequest::Type::NEW_EMPTY: {
			m_asset_loader->cancel();
			auto scene = std::make_unique<GltfScene>(ctx);
			setActiveScene(std::move(scene));
			m_loaded_scene_index = -1;
			m_current_scene_index = -1;
			break;
		}
		case SceneLoadRequest::Type::ADD_MODEL: {
			if (m_active_scene) {
				m_asset_loader->beginModelLoad(m_pending_load.gltf_path, true, m_pending_load.flip_tex_coord_v);
				m_async_load_type = SceneLoadRequest::Type::ADD_MODEL;
			}
			break;
		}
		default:
			break;
	}
	m_pending_load.type = SceneLoadRequest::Type::NONE;
}

void VeApplication::tickAsyncLoader() {
	if (m_asset_loader->getState() == LoadState::IDLE ||
	    m_asset_loader->getState() == LoadState::FAILED)
		return;

	m_asset_loader->tick(&*m_global_pool, &*m_material_set_layout);

	if (m_asset_loader->getState() == LoadState::READY)
		finalizeAsyncLoad();
}

void VeApplication::finalizeAsyncLoad() {
	auto model = m_asset_loader->takeModel();
	if (!model)
		return;

	auto ctx = getSceneContext();
	if (m_async_load_type == SceneLoadRequest::Type::LOAD_REGISTERED) {
		int idx = m_pending_async_scene_index;
		if (idx >= 0 && idx < static_cast<int>(m_scene_entries.size()) && m_scene_entries[static_cast<size_t>(idx)].async_factory) {
			auto scene = m_scene_entries[static_cast<size_t>(idx)].async_factory(ctx, std::move(model));
			setActiveScene(std::move(scene));
			m_loaded_scene_index = idx;
			m_current_scene_index = idx;
		}
		m_pending_async_scene_index = -1;
	} else if (m_async_load_type == SceneLoadRequest::Type::ADD_MODEL && m_active_scene) {
		model->addToScene(m_active_scene->getRegistry(),
		                  {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
		m_event_bus->emitImmediate(AssetLoadCompleteEvent{
			m_asset_loader->getModelName(), {}});
	}
	m_async_load_type = SceneLoadRequest::Type::NONE;
	m_editor->getHierarchyPanel().setLoadTimeDisplay(4.f);
}

// ─── Frame Info Construction ─────────────────────────────────────────────────

void VeApplication::selectBackend() {
	auto& gpu_scene = m_scene_resources->getGpuSceneManager();
	bool gpu_ok = m_ui.gpu_culling_enabled && gpu_scene.hasRegisteredObjects();
	CullingBackend* prev = m_active_backend;

	if (gpu_ok && m_ui.meshlet_culling_enabled
		&& m_meshlet_culling_system
		&& m_scene_resources->getMegaBuffer().hasMeshletData())
		m_active_backend = m_meshlet_backend.get();
	else if (gpu_ok)
		m_active_backend = m_gpu_backend.get();
	else
		m_active_backend = m_cpu_backend.get();

	if (m_active_backend != prev)
		m_event_bus->emitImmediate(BackendChangedEvent{});
}

VeFrameInfo VeApplication::buildFrameInfo() {
	auto& command_buffer = m_ve_renderer.getCurrentCommandBuffer();
	auto& compute_command_buffer = m_ve_renderer.getCurrentComputeCommandBuffer();
	auto& compute2_command_buffer = m_ve_renderer.getCurrentCompute2CommandBuffer();
	auto current_frame = m_ve_renderer.getCurrentFrame();

	vk::raii::DescriptorSet& material_descriptor_set = m_active_scene->getDescriptorSet();
	vk::raii::DescriptorSet& shadow_desc_set = m_shadow_render_system->getShadowDescriptorSet(current_frame);

	int color_space_type = static_cast<int>(m_ve_renderer.getHDRColorMode());

	auto extent = m_ve_renderer.getExtent();
	glm::vec2 texel_size = {1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};

	bool gpu_culling_active = (m_active_backend != m_cpu_backend.get());
	bool meshlet_active = (m_active_backend == m_meshlet_backend.get());

	VeFrameInfo fi = {
		.global_descriptor_set = m_active_backend->getGlobalDescriptorSet(current_frame),
		.texture_descriptor_set = m_particle_descriptor_set,
		.material_descriptor_set = material_descriptor_set,
		.active_scene = m_active_scene.get(),
		.cubemap_descriptor_set = m_skybox_render_system->getCubemapDescriptorSet(),
		.shadow_descriptor_set = shadow_desc_set,
		.command_buffer = &command_buffer,
		.compute_command_buffer = compute_command_buffer,
		.compute2_command_buffer = &compute2_command_buffer,
		.camera = m_camera,
		.registry = &m_active_scene->getRegistry(),
		.visible_objects = m_culling_system->getVisibleObjectsRef(),
		.frame_time = m_frame_time,
		.total_time = m_total_time,
		.current_frame = current_frame,
		.post_process_push = {
			m_ui.blur_radius,
			m_ui.blur_strength,
			m_ui.exposure,
			color_space_type,
			m_ui.bloom_enabled ? m_ui.bloom_strength : 0.0f,
			m_ui.tone_map_mode,
			m_ui.hdr_peak_white,
			0.0f,
			texel_size
		},
		.instance_data = static_cast<InstanceData*>(m_instance_buffers[current_frame]->getMappedMemory()),
		.instance_count = 0,
		.instance_capacity = INITIAL_INSTANCE_CAPACITY,
		.shadow_mode = m_ui.shadow_mode,
		.depth_bias_constant = m_ui.depth_bias_constant,
		.depth_bias_slope = m_ui.depth_bias_slope,
		.depth_bias_clamp = m_ui.depth_bias_clamp,
		.csm_data = {},
		.shadow_atlas_regions = m_shadow_render_system->getAtlasRegions().data(),
		.shadow_atlas_width = m_shadow_render_system->getAtlasWidth(),
		.shadow_atlas_height = m_shadow_render_system->getAtlasHeight(),
		.gpu_culling_active = gpu_culling_active,
		.meshlet_culling_active = meshlet_active,
		.selected_entity = m_editor->getState().selected_entity,
		.cpu_global_descriptor_set = &m_global_descriptor_sets[current_frame],
		.skinning_pre_pass = m_skinning_pre_pass.get(),
	};

	fi.ibl_descriptor_set = &m_ibl_system->getOutputDescriptorSet(current_frame);

	return fi;
}

// ─── Setting Change Detection ────────────────────────────────────────────────

void VeApplication::pushPerFrameSettings() {
	m_ve_renderer.getProfiler().setGpuProfilingEnabled(m_ui.gpu_profiling);
}

void VeApplication::emitSettingEvents() {
	auto extent = m_ve_renderer.getExtent();

	bool depth_bias_changed = m_ui.depth_bias_constant != m_depth_bias_constant
		|| m_ui.depth_bias_slope != m_depth_bias_slope
		|| m_ui.depth_bias_clamp != m_depth_bias_clamp;
	bool topology_changed = m_ui.topology != m_last_topology;
	bool samples_changed = m_ui.pcf_samples != m_pcf_samples
		|| m_ui.pcss_filter_samples != m_pcss_filter_samples;
	bool shadow_mask_res_changed = m_ui.shadow_mask_half_res != m_shadow_mask_half_res;
	bool gtao_res_changed = m_ui.gtao_half_res != m_gtao_half_res;

	// Batch GPU idle for all settings that require pipeline recreation or resource destruction
	if (samples_changed || shadow_mask_res_changed || gtao_res_changed)
		m_ve_device.getDevice().waitIdle();

	if (depth_bias_changed) {
		m_depth_bias_constant = m_ui.depth_bias_constant;
		m_depth_bias_slope = m_ui.depth_bias_slope;
		m_depth_bias_clamp = m_ui.depth_bias_clamp;
		m_event_bus->emitImmediate(DepthBiasChangedEvent{});
	}

	if (topology_changed) {
		m_last_topology = m_ui.topology;
		m_event_bus->emitImmediate(TopologyChangedEvent{.topology = m_ui.topology});
	}

	if (samples_changed) {
		m_pcf_samples = m_ui.pcf_samples;
		m_pcss_filter_samples = m_ui.pcss_filter_samples;
		m_event_bus->emitImmediate(ShadowSamplesChangedEvent{
			.pcf_samples = static_cast<uint32_t>(m_pcf_samples),
			.pcss_filter_samples = static_cast<uint32_t>(m_pcss_filter_samples)
		});
	}

	if (shadow_mask_res_changed) {
		m_shadow_mask_half_res = m_ui.shadow_mask_half_res;
		m_event_bus->emitImmediate(ShadowMaskResolutionChangedEvent{
			.pool = *m_global_pool,
			.mask_extent = halveExtent(extent, m_shadow_mask_half_res),
			.depth_extent = extent,
			.depth_image_view = m_ve_renderer.getResolvedDepthImageView(),
			.depth_image = m_ve_renderer.getResolvedDepthImage()
		});
	}

	if (gtao_res_changed) {
		m_gtao_half_res = m_ui.gtao_half_res;
		m_event_bus->emitImmediate(GtaoResolutionChangedEvent{
			.pool = *m_global_pool,
			.ao_extent = halveExtent(extent, m_gtao_half_res),
			.depth_extent = extent,
			.depth_image_view = m_ve_renderer.getResolvedDepthImageView(),
			.depth_image = m_ve_renderer.getResolvedDepthImage()
		});
	}
}

// ─── UBO Population ──────────────────────────────────────────────────────────

void VeApplication::populateUBO(VeFrameInfo& fi) {
	auto current_frame = fi.current_frame;
	auto extent = m_ve_renderer.getExtent();

	UniformBufferObject ubo{};
	ubo.render_mode = m_ui.render_mode;
	ubo.shadow_mode = m_ui.shadow_mode;
	ubo.pcss_light_size = m_ui.pcss_light_size;
	ubo.shadow_bias = m_ui.shadow_bias;
	ubo.csm_normal_bias = m_ui.csm_normal_bias;
	ubo.csm_blend_dithered = static_cast<uint32_t>(m_ui.csm_blend_mode);
	ubo.ambient_light_color = glm::vec4(m_ui.ambient_light_color, m_ui.ambient_light_intensity);
	m_ui.ibl_exposure_compensation = m_ibl_system->isAvailable() ? m_ibl_system->getExposureCompensation() : 1.0f;
	if (m_ui.ibl_enabled && m_ibl_system->isAvailable()) {
		float comp = m_ui.ibl_auto_exposure ? m_ui.ibl_exposure_compensation : 1.0f;
		ubo.ibl_diffuse_intensity = m_ui.ibl_diffuse_intensity * comp;
		ubo.ibl_specular_intensity = m_ui.ibl_specular_intensity * comp;
	} else {
		ubo.ibl_diffuse_intensity = 0.0f;
		ubo.ibl_specular_intensity = 0.0f;
	}
	ubo.ibl_min_ambient = m_ui.ibl_min_ambient;
	ubo.prefiltered_mip_levels = m_ibl_system->getPrefilteredMipLevels();
	auto& sh = m_ibl_system->getSHCoefficients();
	std::copy(sh.begin(), sh.end(), ubo.sh_coefficients);

	m_light_system->updateUniformBuffer(fi, ubo);
	m_shadow_render_system->updateUniformBuffer(current_frame, ubo, fi.csm_data);

	bool shadow_mask_active = m_ui.shadow_mask_enabled
		&& m_ui.depth_prepass_enabled
		&& m_ui.shadow_mode != ShadowMode::DISABLED;
	ubo.screen_size = glm::vec2(static_cast<float>(extent.width), static_cast<float>(extent.height));

	updateUniformBuffer(current_frame, ubo);

	// Store shadow mask active flag for render
	fi.shadow_mask_active = shadow_mask_active;
}

// ─── Compute Dispatch ────────────────────────────────────────────────────────

void VeApplication::dispatchCompute(VeFrameInfo& fi) {
	ZoneScopedN("Compute Dispatch");
	auto current_frame = fi.current_frame;
	auto extent = m_ve_renderer.getExtent();

	// Upload cluster light data
	uint32_t cluster_light_count = m_cluster_light_system->uploadLightData(fi);
	m_cluster_light_system->setLightCountActive(cluster_light_count > 0);

	// Record compute start timestamp
	m_ve_renderer.getProfiler().beginGpuTimer(fi.compute_command_buffer, ProfileTimer::COMPUTE_TOTAL);

	// Record compute queue commands
	{
		ScopedDebugLabel label(fi.compute_command_buffer, "Compute Dispatch", {0.2f, 0.6f, 0.9f, 1.0f});
		TracyVkZone(m_ve_renderer.getTracyComputeCtx(), *fi.compute_command_buffer, "Compute");
		{
			ZoneScopedN("Skinning Dispatch");
			ScopedDebugLabel skin_label(fi.compute_command_buffer, "Skinning", {0.9f, 0.6f, 0.9f, 1.0f});
			TracyVkZone(m_ve_renderer.getTracyComputeCtx(), *fi.compute_command_buffer, "Skinning");
			m_skinning_pre_pass->dispatch(fi);
		}
		m_fireworks_system->recordComputeCommands(fi);
		m_particle_system->recordComputeCommands(fi);
		if (m_cluster_light_system->isEnabled())
			m_cluster_light_system->dispatch(fi, m_camera, extent);
	}

	m_ve_renderer.submitCompute(fi.compute_command_buffer);

	// Set descriptor sets for downstream render passes
	fi.cluster_descriptor_set = &m_cluster_light_system->getOutputDescriptorSet(current_frame);
}

// ─── Render Frame ────────────────────────────────────────────────────────────

void VeApplication::renderFrame(VeFrameInfo& fi) {
	ZoneScopedN("Render Frame");
	auto& command_buffer = fi.cmd();
	[[maybe_unused]] auto tracy_gfx = m_ve_renderer.getTracyGraphicsCtx();
	[[maybe_unused]] auto tracy_compute = m_ve_renderer.getTracyComputeCtx();

	auto& profiler = m_ve_renderer.getProfiler();
	auto& gpu_scene = m_scene_resources->getGpuSceneManager();
	auto& material_mgr = m_scene_resources->getMaterialManager();
	auto& bindless_set = m_scene_resources->getBindlessRegistry().getDescriptorSet();
	material_mgr.flushToDevice(command_buffer);

	{
		ScopedDebugLabel label(command_buffer, "Culling", {0.6f, 0.6f, 0.6f, 1.0f});
		TracyVkZone(tracy_gfx, *command_buffer, "Culling");
		if (fi.gpu_culling_active)
			profiler.beginGpuTimer(command_buffer, ProfileTimer::CULLING);
		else
			profiler.beginCpuTimer(ProfileTimer::CULLING);
		m_active_backend->cull(fi, gpu_scene);
		m_pbr_render_system->prepareSkinnedFrame(fi, material_mgr);
		if (fi.gpu_culling_active)
			profiler.endGpuTimer(command_buffer, ProfileTimer::CULLING);
		else
			profiler.endCpuTimer(ProfileTimer::CULLING);
	}

	bool editor_mode = m_editor->isEditorMode();

	bool hiz_active = fi.gpu_culling_active && m_active_backend->isHizEnabled()
		&& m_ui.depth_prepass_enabled;
	bool gtao_active = m_ui.gtao_enabled && m_ui.depth_prepass_enabled;

	// Depth prepass (enables depth consumers below)
	if (m_ui.depth_prepass_enabled) {
		ZoneScopedN("Depth Prepass");
		TracyVkZone(tracy_gfx, *command_buffer, "Depth Prepass");
		profiler.beginCpuTimer(ProfileTimer::DEPTH_PREPASS);
		profiler.beginGpuTimer(command_buffer, ProfileTimer::DEPTH_PREPASS);
		m_ve_renderer.beginDepthPrePass(command_buffer);
		m_active_backend->renderDepthPrePass(fi, m_scene_resources->getMegaBuffer(),
			*m_depth_prepass_system);
		m_depth_prepass_system->renderSkinned(fi, m_pbr_render_system->getSkinnedDrawables());
		m_ve_renderer.endDepthPrePass(command_buffer);
		profiler.endGpuTimer(command_buffer, ProfileTimer::DEPTH_PREPASS);
		profiler.endCpuTimer(ProfileTimer::DEPTH_PREPASS);
	}

	// Depth read-only consumers: Shadow Mask, GTAO + Hi-Z
	bool shadows_enabled = m_ui.shadow_mode != ShadowMode::DISABLED;
	bool any_depth_consumer = m_ui.depth_prepass_enabled
		&& (hiz_active || fi.shadow_mask_active || gtao_active);
	bool any_async_consumer = m_ve_device.hasDedicatedComputeQueue()
		&& (gtao_active || hiz_active);

	// Non-async path: shadows run on graphics primary before depth consumers
	if (shadows_enabled && !any_async_consumer) {
		ScopedDebugLabel label(command_buffer, "Shadow Maps", {0.5f, 0.2f, 0.2f, 1.0f});
		ZoneScopedN("Shadow Maps");
		TracyVkZone(tracy_gfx, *command_buffer, "Shadow Maps");
		profiler.beginCpuTimer(ProfileTimer::SHADOW_MAPS);
		profiler.beginGpuTimer(command_buffer, ProfileTimer::SHADOW_MAPS);
		m_active_backend->renderShadows(fi, *m_shadow_render_system,
			m_scene_resources->getMegaBuffer(), gpu_scene);
		profiler.endGpuTimer(command_buffer, ProfileTimer::SHADOW_MAPS);
		profiler.endCpuTimer(ProfileTimer::SHADOW_MAPS);
	}

	if (any_depth_consumer) {
		// Depth attachment -> read-only
		vk::ImageMemoryBarrier2 depth_to_read{
			.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
			.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
			.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_ve_renderer.getResolvedDepthImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
		};
		vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &depth_to_read};
		command_buffer.pipelineBarrier2(dep);

		if (any_async_consumer) {
			// Submit depth-only on graphics CB 1 (no shadows yet)
			m_ve_renderer.submitGraphicsPhase1();

			// Record GTAO + Hi-Z on compute CB 2 (waits depth_done via semaphore)
			auto& compute2_cb = *fi.compute2_command_buffer;

			if (gtao_active) {
				ScopedDebugLabel label(compute2_cb, "GTAO", {0.2f, 0.7f, 0.7f, 1.0f});
				ZoneScopedN("GTAO");
				TracyVkZone(tracy_compute, *compute2_cb, "GTAO (async)");
				profiler.beginCpuTimer(ProfileTimer::GTAO);
				profiler.beginGpuTimer(compute2_cb, ProfileTimer::GTAO);
				m_gtao_system->dispatch(fi, compute2_cb);
				profiler.endGpuTimer(compute2_cb, ProfileTimer::GTAO);
				profiler.endCpuTimer(ProfileTimer::GTAO);
				fi.ao_descriptor_set = &m_gtao_system->getOutputDescriptorSet(fi.current_frame);
			}

			if (hiz_active) {
				ScopedDebugLabel label(compute2_cb, "Hi-Z", {0.4f, 0.4f, 0.8f, 1.0f});
				TracyVkZone(tracy_compute, *compute2_cb, "Hi-Z (async)");
				profiler.beginCpuTimer(ProfileTimer::HIZ);
				profiler.beginGpuTimer(compute2_cb, ProfileTimer::HIZ);
				m_hiz_system->generate(compute2_cb, fi.current_frame);
				profiler.endGpuTimer(compute2_cb, ProfileTimer::HIZ);
				profiler.endCpuTimer(ProfileTimer::HIZ);
			}

			// Record shadows + shadow mask on graphics CB 2
			auto& gfx2_cb = m_ve_renderer.getCurrentGraphics2CommandBuffer();
			fi.command_buffer = &gfx2_cb;

			if (shadows_enabled) {
				ScopedDebugLabel label(gfx2_cb, "Shadow Maps", {0.5f, 0.2f, 0.2f, 1.0f});
				ZoneScopedN("Shadow Maps");
				TracyVkZone(tracy_gfx, *gfx2_cb, "Shadow Maps");
				profiler.beginCpuTimer(ProfileTimer::SHADOW_MAPS);
				profiler.beginGpuTimer(gfx2_cb, ProfileTimer::SHADOW_MAPS);
				m_active_backend->renderShadows(fi, *m_shadow_render_system,
					m_scene_resources->getMegaBuffer(), gpu_scene);
				profiler.endGpuTimer(gfx2_cb, ProfileTimer::SHADOW_MAPS);
				profiler.endCpuTimer(ProfileTimer::SHADOW_MAPS);
			}

			if (fi.shadow_mask_active) {
				ScopedDebugLabel label(gfx2_cb, "Shadow Mask", {0.5f, 0.3f, 0.5f, 1.0f});
				TracyVkZone(tracy_gfx, *gfx2_cb, "Shadow Mask");
				profiler.beginCpuTimer(ProfileTimer::SHADOW_MASK);
				profiler.beginGpuTimer(gfx2_cb, ProfileTimer::SHADOW_MASK);
				m_shadow_mask_system->dispatch(fi);
				profiler.endGpuTimer(gfx2_cb, ProfileTimer::SHADOW_MASK);
				profiler.endCpuTimer(ProfileTimer::SHADOW_MASK);
				fi.shadow_mask_descriptor_set = &m_shadow_mask_system->getOutputDescriptorSet(fi.current_frame);
			}

			// Submit shadow CB 2 (no semaphore, same-queue ordering after depth CB 1)
			m_ve_renderer.submitShadowPhase(gfx2_cb);

			// Submit compute CB 2 (waits graphics1/depth, signals compute2)
			m_ve_renderer.submitComputePhase2(compute2_cb);

			// Switch to graphics CB 3 for scene render (waits compute2 at eFragmentShader)
			// Same queue guarantees shadows+mask (CB 2) are complete
			auto& gfx3_cb = m_ve_renderer.getCurrentGraphics3CommandBuffer();
			fi.command_buffer = &gfx3_cb;

			// Depth read-only -> attachment on graphics CB 3
			// Within-queue sync: shadow mask (CB 2) read complete (same-queue ordering)
			// Cross-queue sync (GTAO/Hi-Z): semaphore wait at eFragmentShader
			vk::ImageMemoryBarrier2 depth_to_attach{
				.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
				.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
				.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
				.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead
					| vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
				.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
				.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_ve_renderer.getResolvedDepthImage(),
				.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
			};
			vk::DependencyInfo dep2{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &depth_to_attach};
			gfx3_cb.pipelineBarrier2(dep2);
		} else {
			// No async split: shadow mask, GTAO, Hi-Z all on graphics CB 1
			if (fi.shadow_mask_active) {
				ScopedDebugLabel label(command_buffer, "Shadow Mask", {0.5f, 0.3f, 0.5f, 1.0f});
				TracyVkZone(tracy_gfx, *command_buffer, "Shadow Mask");
				profiler.beginCpuTimer(ProfileTimer::SHADOW_MASK);
				profiler.beginGpuTimer(command_buffer, ProfileTimer::SHADOW_MASK);
				m_shadow_mask_system->dispatch(fi);
				profiler.endGpuTimer(command_buffer, ProfileTimer::SHADOW_MASK);
				profiler.endCpuTimer(ProfileTimer::SHADOW_MASK);
				fi.shadow_mask_descriptor_set = &m_shadow_mask_system->getOutputDescriptorSet(fi.current_frame);
			}

			if (gtao_active) {
				ScopedDebugLabel label(command_buffer, "GTAO", {0.2f, 0.7f, 0.7f, 1.0f});
				ZoneScopedN("GTAO");
				TracyVkZone(tracy_gfx, *command_buffer, "GTAO");
				profiler.beginCpuTimer(ProfileTimer::GTAO);
				profiler.beginGpuTimer(command_buffer, ProfileTimer::GTAO);
				m_gtao_system->dispatch(fi, command_buffer);
				profiler.endGpuTimer(command_buffer, ProfileTimer::GTAO);
				profiler.endCpuTimer(ProfileTimer::GTAO);
				fi.ao_descriptor_set = &m_gtao_system->getOutputDescriptorSet(fi.current_frame);
			}

			if (hiz_active) {
				ScopedDebugLabel label(command_buffer, "Hi-Z", {0.4f, 0.4f, 0.8f, 1.0f});
				TracyVkZone(tracy_gfx, *command_buffer, "Hi-Z Build");
				profiler.beginCpuTimer(ProfileTimer::HIZ);
				profiler.beginGpuTimer(command_buffer, ProfileTimer::HIZ);
				m_hiz_system->generate(command_buffer, fi.current_frame);
				profiler.endGpuTimer(command_buffer, ProfileTimer::HIZ);
				profiler.endCpuTimer(ProfileTimer::HIZ);
			}

			vk::ImageMemoryBarrier2 depth_to_attach{
				.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
				.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
				.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
				.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead
					| vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
				.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
				.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_ve_renderer.getResolvedDepthImage(),
				.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
			};
			vk::DependencyInfo dep2{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &depth_to_attach};
			command_buffer.pipelineBarrier2(dep2);
		}
	}

	// Set default descriptor sets for inactive passes
	if (!fi.shadow_mask_active)
		fi.shadow_mask_descriptor_set = &m_shadow_mask_system->getDummyOutputDescriptorSet();
	if (!gtao_active)
		fi.ao_descriptor_set = &m_gtao_system->getDummyOutputDescriptorSet();

	// From this point on, fi.cmd() is the active graphics CB (may be CB 1 or CB 2)
	auto& active_cb = fi.cmd();

	// Scene render
	{
		ZoneScopedN("Scene Render");
		TracyVkZone(tracy_gfx, *active_cb, "Scene Render");
		profiler.beginCpuTimer(ProfileTimer::SCENE_RENDER);
		profiler.beginGpuTimer(active_cb, ProfileTimer::SCENE_RENDER);
		m_ve_renderer.beginSceneRender(active_cb, m_ui.depth_prepass_enabled);
		m_active_backend->renderOpaque(fi, *m_pbr_render_system, bindless_set);
		m_pbr_render_system->renderSkinned(fi, *m_skinning_pre_pass, bindless_set);
		m_skybox_render_system->render(fi);
		if (!fi.gpu_culling_active)
			m_pbr_render_system->renderTransparent(fi, bindless_set);
		m_particle_system->render(fi);
		if (m_ui.show_axes)
			m_axes_render_system->render(fi);
		if (m_ui.show_aabb_debug)
			m_aabb_debug_render_system->render(fi);
		if (m_ui.show_skinned_points)
			m_skinned_points_render_system->render(fi, *m_skinning_pre_pass);
		m_light_system->render(fi);
		m_fireworks_system->render(fi);
		m_ve_renderer.endSceneRender(active_cb);
		profiler.endGpuTimer(active_cb, ProfileTimer::SCENE_RENDER);
		profiler.endCpuTimer(ProfileTimer::SCENE_RENDER);
	}

	// Transparency (CPU sort, WBOIT, or no-op depending on backend)
	m_active_backend->renderTransparency(fi, *m_pbr_render_system, bindless_set,
		gpu_scene, m_ve_renderer);

	// Selection outline: mask + JFA
	auto& editor_state = m_editor->getState();
	bool outline_active = editor_state.outline_enabled && !fi.selected_entity.isNull();
	if (outline_active) {
		ScopedDebugLabel label(active_cb, "Selection Outline", {1.0f, 0.5f, 0.0f, 1.0f});
		profiler.beginCpuTimer(ProfileTimer::OUTLINE);
		profiler.beginGpuTimer(active_cb, ProfileTimer::OUTLINE);
		m_outline_system->renderMask(fi, *fi.registry, fi.selected_entity);
		if (m_outline_system->hasOutline())
			m_outline_system->dispatchJFA(fi, editor_state.outline_width);
		profiler.endGpuTimer(active_cb, ProfileTimer::OUTLINE);
		profiler.endCpuTimer(ProfileTimer::OUTLINE);
	}

	// Bloom
	if (m_ui.bloom_enabled) {
		ScopedDebugLabel label(active_cb, "Bloom", {1.0f, 0.8f, 0.3f, 1.0f});
		ZoneScopedN("Bloom");
		TracyVkZone(tracy_gfx, *active_cb, "Bloom");
		profiler.beginCpuTimer(ProfileTimer::BLOOM);
		profiler.beginGpuTimer(active_cb, ProfileTimer::BLOOM);
		m_bloom_system->render(active_cb);
		profiler.endGpuTimer(active_cb, ProfileTimer::BLOOM);
		profiler.endCpuTimer(ProfileTimer::BLOOM);
	}

	// Post-processing + outline composite
	{
		ZoneScopedN("Post Process");
		TracyVkZone(tracy_gfx, *active_cb, "Post Process");
		profiler.beginCpuTimer(ProfileTimer::POST_PROCESS);
		profiler.beginGpuTimer(active_cb, ProfileTimer::POST_PROCESS);
		m_ve_renderer.beginPostProcessRender(active_cb, editor_mode);
		m_post_process_system->render(active_cb, fi.post_process_push);
		if (outline_active && m_outline_system->hasOutline())
			m_outline_system->composite(active_cb, fi.current_frame,
				editor_state.outline_width, editor_state.outline_color);
		m_ve_renderer.endPostProcessRender(active_cb, editor_mode);
		profiler.endGpuTimer(active_cb, ProfileTimer::POST_PROCESS);
		profiler.endCpuTimer(ProfileTimer::POST_PROCESS);
	}
}

// ─── Stats Collection ────────────────────────────────────────────────────────

void VeApplication::collectStats(const VeFrameInfo& fi) {
	auto& profiler = m_ve_renderer.getProfiler();
	profiler.endCpuTimer(ProfileTimer::FRAME_TOTAL);

	// Scene stats
	auto& registry = m_active_scene->getRegistry();
	m_active_backend->collectStats(fi.current_frame, m_ui, registry);
	m_ui.stats.num_point_lights = registry.activePointLightCount();
	m_ui.stats.num_directional_lights = registry.activeDirectionalLightCount();
	m_ui.stats.num_spot_lights = registry.activeSpotLightCount();

	const auto& results = profiler.getResults();
	m_ui.stats.fence_wait = results.fence_wait_ms;
	m_ui.stats.acquire_wait = results.acquire_wait_ms;
	m_ui.stats.cpu_time = results.cpu(ProfileTimer::FRAME_TOTAL) - results.fence_wait_ms - results.acquire_wait_ms;
	m_ui.stats.gpu_time = results.gpu(ProfileTimer::FRAME_TOTAL);
	m_ui.stats.compute_gpu_time = results.gpu(ProfileTimer::COMPUTE_TOTAL);
	m_ui.stats.gpu_overlap = results.gpu_overlap;

	// Per-system GPU breakdown
	m_ui.stats.gpu_culling = results.gpu(ProfileTimer::CULLING);
	m_ui.stats.gpu_shadow_maps = results.gpu(ProfileTimer::SHADOW_MAPS);
	m_ui.stats.gpu_depth_prepass = results.gpu(ProfileTimer::DEPTH_PREPASS);
	m_ui.stats.gpu_gtao = results.gpu(ProfileTimer::GTAO);
	m_ui.stats.gpu_scene_render = results.gpu(ProfileTimer::SCENE_RENDER);
	m_ui.stats.gpu_bloom = results.gpu(ProfileTimer::BLOOM);
	m_ui.stats.gpu_post_process = results.gpu(ProfileTimer::POST_PROCESS);
	m_ui.stats.gpu_hiz = results.gpu(ProfileTimer::HIZ);
	m_ui.stats.gpu_shadow_mask = results.gpu(ProfileTimer::SHADOW_MASK);
	m_ui.stats.gpu_outline = results.gpu(ProfileTimer::OUTLINE);

	// Per-system CPU breakdown
	m_ui.stats.cpu_culling = results.cpu(ProfileTimer::CULLING);
	m_ui.stats.cpu_shadow_maps = results.cpu(ProfileTimer::SHADOW_MAPS);
	m_ui.stats.cpu_depth_prepass = results.cpu(ProfileTimer::DEPTH_PREPASS);
	m_ui.stats.cpu_gtao = results.cpu(ProfileTimer::GTAO);
	m_ui.stats.cpu_scene_render = results.cpu(ProfileTimer::SCENE_RENDER);
	m_ui.stats.cpu_bloom = results.cpu(ProfileTimer::BLOOM);
	m_ui.stats.cpu_post_process = results.cpu(ProfileTimer::POST_PROCESS);
	m_ui.stats.cpu_hiz = results.cpu(ProfileTimer::HIZ);
	m_ui.stats.cpu_shadow_mask = results.cpu(ProfileTimer::SHADOW_MASK);
	m_ui.stats.cpu_outline = results.cpu(ProfileTimer::OUTLINE);
	m_ui.stats.cpu_physics = results.cpu(ProfileTimer::PHYSICS);
}

// ─── Swap Chain Recreation ───────────────────────────────────────────────────

void VeApplication::onSwapChainRecreated() {
	m_ve_device.assertDeviceIdle();
	m_event_bus->emitImmediate(PipelineRecreateEvent{
		.offscreen_format = m_ve_renderer.getOffscreenImageFormat(),
		.sample_count = m_ve_renderer.getSampleCount()
	});
	auto extent = m_ve_renderer.getExtent();
	m_event_bus->emitImmediate(ResolutionChangedEvent{
		.pool = *m_global_pool,
		.extent = extent,
		.swap_chain_format = m_ve_renderer.getSwapChainImageFormat(),
		.offscreen_format = m_ve_renderer.getOffscreenImageFormat(),
		.resolve_target_view = m_ve_renderer.getResolveTargetImageView(),
		.depth_image_view = m_ve_renderer.getResolvedDepthImageView(),
		.depth_image = m_ve_renderer.getResolvedDepthImage(),
		.wboit_accum_view = m_ve_renderer.getWboitAccumImageView(),
		.wboit_revealage_view = m_ve_renderer.getWboitRevealageImageView(),
		.shadow_mask_half_res = m_shadow_mask_half_res,
		.gtao_half_res = m_gtao_half_res
	});
	m_imgui_layer->recreatePipeline();
	m_editor->onSwapChainRecreated();
}

void VeApplication::recreateResolutionDependentSystems() {
	auto extent = m_ve_renderer.getExtent();
	m_event_bus->emitImmediate(ResolutionChangedEvent{
		.pool = *m_global_pool,
		.extent = extent,
		.swap_chain_format = m_ve_renderer.getSwapChainImageFormat(),
		.offscreen_format = m_ve_renderer.getOffscreenImageFormat(),
		.resolve_target_view = m_ve_renderer.getResolveTargetImageView(),
		.depth_image_view = m_ve_renderer.getResolvedDepthImageView(),
		.depth_image = m_ve_renderer.getResolvedDepthImage(),
		.wboit_accum_view = m_ve_renderer.getWboitAccumImageView(),
		.wboit_revealage_view = m_ve_renderer.getWboitRevealageImageView(),
		.shadow_mask_half_res = m_shadow_mask_half_res,
		.gtao_half_res = m_gtao_half_res
	});
}

// ─── Buffer / Descriptor / System Initialization ─────────────────────────────

void VeApplication::createBuffers() {
	VE_LOGD("Creating buffers");
	vk::DeviceSize buffer_size = sizeof(UniformBufferObject);
	assert(buffer_size > 0 && "Uniform buffer size is zero");
	assert(buffer_size <= m_ve_device.getDeviceProperties().limits.maxUniformBufferRange && "Uniform buffer size exceeds maximum limit");

	m_uniform_buffers.clear();
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_uniform_buffers.emplace_back(std::make_unique<VeBuffer>(
			m_ve_device, buffer_size, 1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
		));
		m_uniform_buffers[i]->map();
	}

	m_instance_buffers.clear();
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_instance_buffers.emplace_back(std::make_unique<VeBuffer>(
			m_ve_device, sizeof(InstanceData), INITIAL_INSTANCE_CAPACITY,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		));
		m_instance_buffers[i]->map();
	}
}

void VeApplication::createDescriptors() {
	VE_LOGD("Creating descriptors");

	// Global set layout: UBO (binding 0), instance SSBO (binding 1), material SSBO (binding 2)
	m_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics | vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment)
		.build();

	// Material set layout: albedo, normal, metallic-roughness, occlusion, emissive, UBO
	m_material_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(2, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(3, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(4, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(5, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eFragment)
		.build();

	// Set default page sizes of the pool.
	// If a page fills up, a new one is created automatically.
	m_global_pool = VeDescriptorPool::Builder(m_ve_device)
		.setMaxSets(1024)
		.addPoolSize(vk::DescriptorType::eUniformBuffer, 1024)
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, 4096)
		.addPoolSize(vk::DescriptorType::eSampler, 256)
		.addPoolSize(vk::DescriptorType::eSampledImage, 256)
		.addPoolSize(vk::DescriptorType::eStorageBuffer, 1024)
		.addPoolSize(vk::DescriptorType::eStorageImage, 128)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
		.buildShared();

	// Global descriptor sets (per frame)
	m_global_descriptor_sets.clear();
	m_global_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);
	auto material_ssbo_info = m_scene_resources->getMaterialManager().getBuffer().getDescriptorInfo();
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto buffer_info = m_uniform_buffers[i]->getDescriptorInfo();
		auto instance_info = m_instance_buffers[i]->getDescriptorInfo();
		vk::raii::DescriptorSet set{nullptr};
		VeDescriptorWriter(*m_global_set_layout, *m_global_pool)
			.writeBuffer(0, &buffer_info)
			.writeBuffer(1, &instance_info)
			.writeBuffer(2, &material_ssbo_info)
			.build(set);
		m_global_descriptor_sets.push_back(std::move(set));
	}

	// Default material UBO
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

	// Particle textures
	m_particle_texture_handle = m_resource_manager->load<VeTexture>(m_config.particle_texture.lexically_normal().generic_string());
	m_fire_texture_handle = m_resource_manager->load<VeTexture>(m_config.fire_texture.lexically_normal().generic_string());
	m_smoke_texture_handle = m_resource_manager->load<VeTexture>(m_config.smoke_texture.lexically_normal().generic_string());
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

	// Default material descriptor set
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

void VeApplication::initSystems() {
	VE_LOGD("Initialising systems");
	auto shader = [this](const char* name) { return m_config.shaders_dir / name; };

	// Register engine-level input actions
	m_input_controller.setEventBus(m_event_bus.get());
	m_input_controller.registerAction({
		.name = "Toggle Performance UI",
		.key = GLFW_KEY_P,
		.trigger = TriggerType::OnPress,
		.context = InputContext::Always,
		.description = "Toggle performance panel"
	});
	m_event_bus->subscribe<InputActionEvent>([this](const InputActionEvent& e) {
		if (e.name == "Toggle Performance UI")
			m_editor->getState().show_performance = !m_editor->getState().show_performance;
	});

	m_culling_system = std::make_unique<CullingSystem>(m_camera);

	m_shadow_render_system = std::make_unique<ShadowRenderSystem>(
		m_ve_device, *m_global_pool,
		m_material_set_layout->getDescriptorSetLayout(),
		shader("shadow_shader.spv"),
		*m_event_bus
	);

	m_depth_prepass_system = std::make_unique<DepthPrePassSystem>(
		m_ve_device, m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSampleCount(), shader("depth_prepass_shader.spv"),
		*m_event_bus
	);

	m_shadow_mask_system = std::make_unique<ShadowMaskSystem>(
		m_ve_device, *m_global_pool, *m_resource_manager,
		m_global_set_layout->getDescriptorSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_config.shaders_dir, halveExtent(m_ve_renderer.getExtent(), m_ui.shadow_mask_half_res),
		m_ve_renderer.getExtent(),
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage(),
		*m_event_bus
	);
	m_shadow_mask_half_res = m_ui.shadow_mask_half_res;

	m_gtao_system = std::make_unique<GtaoSystem>(
		m_ve_device, *m_global_pool, *m_resource_manager,
		m_global_set_layout->getDescriptorSetLayout(),
		m_config.shaders_dir, halveExtent(m_ve_renderer.getExtent(), m_ui.gtao_half_res),
		m_ve_renderer.getExtent(),
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage(),
		*m_event_bus
	);
	m_gtao_half_res = m_ui.gtao_half_res;

	m_cluster_light_system = std::make_unique<ClusterLightSystem>(
		m_ve_device, *m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		shader("cluster_assign_comp.spv"), m_ve_renderer.getExtent(),
		*m_event_bus
	);

	// IblSystem subscribes to SkyboxChangedEvent (must be constructed before SkyboxRenderSystem)
	m_ibl_system = std::make_unique<IblSystem>(
		m_ve_device, *m_global_pool, *m_resource_manager,
		m_config.skybox_dir / "brdf_lut.ktx",
		*m_event_bus
	);

	m_pbr_render_system = std::make_unique<PbrRenderSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_scene_resources->getBindlessRegistry().getSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_shadow_mask_system->getShadowMaskSetLayout(),
		m_cluster_light_system->getOutputSetLayout(),
		m_gtao_system->getAoSetLayout(),
		m_ibl_system->getIblSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount(),
		shader("pbr_shader.spv"),
		*m_event_bus,
		m_scene_resources->getMegaBuffer()
	);
	m_pbr_render_system->initWboit(
		m_ve_renderer.getWboitAccumImageView(),
		m_ve_renderer.getWboitRevealageImageView(),
		m_ve_renderer.getOffscreenImageFormat());

	m_aabb_debug_render_system = std::make_unique<AabbDebugRenderSystem>(
		m_ve_device, m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("axes_shader.spv"),
		*m_event_bus
	);

	m_axes_render_system = std::make_unique<AxesRenderSystem>(
		m_ve_device, *m_resource_manager,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("axes_shader.spv"),
		*m_event_bus
	);

	m_light_system = std::make_unique<LightSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("light_billboard_shader.spv"),
		*m_event_bus
	);

	m_particle_system = std::make_unique<ParticleSystem>(
		m_ve_device, m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		50000, glm::vec3{0.0f, -300.0f, 50.0f},
		shader("particle_compute.spv"),
		true, m_event_bus.get()
	);

	m_fireworks_system = std::make_unique<FireworksSystem>(
		m_ve_device, m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("particle_compute.spv"),
		*m_event_bus
	);

	m_skybox_render_system = std::make_unique<SkyboxRenderSystem>(
		m_ve_device, *m_resource_manager, *m_global_pool, *m_material_set_layout,
		m_global_set_layout->getDescriptorSetLayout(),
		m_config.skybox_dir, shader("skybox_shader.spv"), m_config.cube_model,
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		*m_event_bus
	);

	m_bloom_system = std::make_unique<BloomSystem>(
		m_ve_device, m_ve_renderer.getExtent(),
		m_ve_renderer.getResolveTargetImageView(),
		shader("bloom_downsample.spv"), shader("bloom_upsample.spv"),
		*m_event_bus
	);

	m_post_process_system = std::make_unique<PostProcessSystem>(
		m_ve_device, m_ve_renderer.getSwapChainImageFormat(),
		m_ve_renderer.getResolveTargetImageView(),
		m_bloom_system->getBloomTexture(),
		shader("post_process.spv"),
		*m_event_bus, *m_bloom_system
	);

	m_outline_system = std::make_unique<OutlineSystem>(
		m_ve_device, *m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_config.shaders_dir,
		m_ve_renderer.getExtent(),
		m_ve_renderer.getSwapChainImageFormat(),
		*m_event_bus
	);

	// GPU-driven culling
	auto& gpu_scene = m_scene_resources->getGpuSceneManager();
	m_gpu_culling_system = std::make_unique<GpuCullingSystem>(m_ve_device, m_config.shaders_dir);
	m_gpu_culling_system->createDescriptorSets(*m_global_pool, gpu_scene);
	m_gpu_culling_system->createShadowDescriptorSets(*m_global_pool, gpu_scene);
	if (m_gpu_culling_system->compactionEnabled())
		m_gpu_culling_system->createCompactionDescriptorSets(*m_global_pool);
	m_gpu_culling_system->createGlobalDescriptorSets(*m_global_pool, *m_global_set_layout,
		m_uniform_buffers, m_scene_resources->getMaterialManager().getBuffer());

	m_shadow_render_system->createGpuShadowDescriptorSets(*m_gpu_culling_system);

	// Hi-Z occlusion culling
	m_hiz_system = std::make_unique<HizSystem>(
		m_ve_device, *m_global_pool, m_ve_renderer.getExtent(),
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage(),
		m_config.shaders_dir, *m_event_bus);
	m_gpu_culling_system->createHizDescriptorSets(*m_global_pool, gpu_scene, *m_hiz_system);
	m_gpu_culling_system->createShadowHizDescriptorSets(*m_global_pool, gpu_scene, *m_hiz_system);
	m_gpu_culling_system->subscribeToEvents(*m_event_bus, *m_hiz_system, gpu_scene);

	// SceneResourceManager subscribes to scene lifecycle events.
	// Must be before MeshletCullingSystem so the mega buffer is rebuilt before
	// meshlet descriptors reference it.
	m_scene_resources->subscribeToEvents(*m_event_bus);

	// Meshlet culling (subscribes to SceneLoadedEvent/AssetLoadCompleteEvent for descriptor rebuild)
	m_meshlet_culling_system = std::make_unique<MeshletCullingSystem>(
		m_ve_device, m_config.shaders_dir, *m_event_bus, *m_global_pool,
		*m_scene_resources, m_scene_resources->getMegaBuffer(), *m_hiz_system);
	m_meshlet_culling_system->createDescriptorSets(*m_global_pool,
		gpu_scene, m_scene_resources->getMegaBuffer());
	m_meshlet_culling_system->createHizDescriptorSets(*m_global_pool,
		gpu_scene, m_scene_resources->getMegaBuffer(), *m_hiz_system);
	m_meshlet_culling_system->createGlobalDescriptorSets(*m_global_pool, *m_global_set_layout,
		m_uniform_buffers, m_scene_resources->getMaterialManager().getBuffer());
	m_meshlet_culling_system->createShadowDescriptorSets(*m_global_pool,
		gpu_scene, m_scene_resources->getMegaBuffer());
	m_shadow_render_system->createMeshletShadowDescriptorSets(*m_meshlet_culling_system);

	m_skinning_pre_pass = std::make_unique<SkinningPrePass>(
		m_ve_device, *m_global_pool, shader("skinning_comp.spv"), *m_event_bus);

	m_skinned_points_render_system = std::make_unique<SkinnedPointsRenderSystem>(
		m_ve_device, m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("skinned_points.spv"), *m_event_bus);

	// Culling backends
	m_cpu_backend = std::make_unique<CpuCullingBackend>(
		*m_culling_system, *m_pbr_render_system,
		m_scene_resources->getMaterialManager(), m_ui,
		m_global_descriptor_sets, m_ve_renderer.getThreadPool());
	m_gpu_backend = std::make_unique<GpuCullingBackend>(
		*m_gpu_culling_system, gpu_scene);
	m_meshlet_backend = std::make_unique<MeshletCullingBackend>(
		*m_meshlet_culling_system, *m_gpu_culling_system, gpu_scene, *m_event_bus);
	m_active_backend = m_cpu_backend.get();

	// Physics (subscribes to SceneLoaded/Unloaded/AssetLoadComplete internally)
	m_physics_system = std::make_unique<PhysicsSystem>();
	m_physics_system->setEventBus(m_event_bus.get());

	// Swap chain recreation on topology change (renderer-level, not owned by PbrRenderSystem)
	m_event_bus->subscribe<TopologyChangedEvent>([this](const TopologyChangedEvent&) {
		m_ve_renderer.setSwapChainNeedsRecreation();
	});
}

void VeApplication::initEditor() {
	VE_LOGD("Initialising UI");
	m_imgui_layer = std::make_unique<ImGuiLayer>(m_ve_window, m_ve_device, m_ve_renderer);
	m_imgui_layer->setAppSettingsWindowName(m_config.app_name);
	m_editor = std::make_unique<Editor>(m_ve_renderer, *m_imgui_layer, *m_event_bus);
	m_editor->setAppUICallback([this]() { renderUI(); });

	// Wire scene registry and systems into editor
	m_editor->setSceneRegistry(&m_scene_entries, &m_current_scene_index, &m_pending_load);
	m_editor->setSkyboxSystem(m_skybox_render_system.get());
	m_editor->setShadowRenderSystem(m_shadow_render_system.get());
	m_editor->setPhysicsSystem(m_physics_system.get());
	m_editor->setAssetLoader(m_asset_loader.get());
	m_editor->setCamera(&m_camera);
	m_editor->setInputController(&m_input_controller);

	m_ui.hdr_enabled = m_ve_renderer.hasHdrSupport() && m_ve_renderer.isHdrEnabled();
	m_ui.fov = glm::degrees(m_fov);
	m_ui.ambient_light_color = glm::vec3(DEFAULT_AMBIENT_LIGHT_COLOR);
	m_ui.ambient_light_intensity = DEFAULT_AMBIENT_LIGHT_COLOR.w;
}

// ─── Camera ──────────────────────────────────────────────────────────────────

void VeApplication::updateCamera(float fov_radians) {
	m_camera.updateIfDirty();
	float aspect = m_ve_renderer.getExtentAspectRatio();
	bool aspect_changed = aspect > 0.0f && std::abs(aspect - m_last_aspect) > std::numeric_limits<float>::epsilon();
	bool fov_changed = std::abs(fov_radians - m_fov) > 1e-4f;
	if (aspect_changed || fov_changed) {
		m_last_aspect = aspect;
		m_fov = fov_radians;
		m_camera.setPerspective(m_fov, m_last_aspect, m_near_plane, m_far_plane);
	}
}

void VeApplication::updateUniformBuffer(uint32_t current_frame, UniformBufferObject& ubo) {
	ubo.view = m_camera.getView();
	ubo.proj = m_camera.getProj();
	ubo.projection_view = ubo.proj * ubo.view;
	ubo.inverse_projection_view = glm::inverse(ubo.projection_view);
	ubo.prev_projection_view = m_prev_projection_view;
	m_prev_projection_view = ubo.projection_view;
	ubo.camera_position = glm::vec4{m_camera.getPosition(), 1.0f};
	m_uniform_buffers[current_frame]->writeToBuffer(&ubo);
}

void VeApplication::updateFrameTime() {
	auto now = clock::now();
	m_frame_time = std::chrono::duration<float, std::chrono::seconds::period>(now - m_last_frame_time).count();
	m_last_frame_time = now;
	const float max_dt = 1.0f / 10.0f;
	if (m_frame_time < 0.0f)
		m_frame_time = 0.0f;
	if (m_frame_time > max_dt)
		m_frame_time = max_dt;
}

void VeApplication::setWindowTitle() {
#ifdef NDEBUG
	const char* mode_str = "Release";
#else
	const char* mode_str = "Debug";
#endif
	std::string title = std::format("Vulkan Engine -- {} mode", mode_str);
	glfwSetWindowTitle(m_ve_window.getGLFWwindow(), title.c_str());
}

} // namespace ve
