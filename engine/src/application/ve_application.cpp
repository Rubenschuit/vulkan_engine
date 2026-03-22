#include "application/ve_application.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_device.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/editor.hpp"
#include "vulkan/ve_buffer.hpp"
#include "input/input_controller.hpp"
#include "scene/ve_camera.hpp"
#include "scene/ve_scene.hpp"
#include "scene/gltf_scene.hpp"
#include "utils/ve_log.hpp"

// Render systems
#include "rendering/culling_system.hpp"
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
#include "rendering/scene_resource_manager.hpp"
#include "rendering/bindless_texture_registry.hpp"
#include "rendering/material_ssbo_manager.hpp"
#include "rendering/gpu_scene_manager.hpp"
#include "rendering/gpu_culling_system.hpp"
#include "rendering/meshlet_culling_system.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/cpu_culling_backend.hpp"
#include "rendering/gpu_culling_backend.hpp"
#include "rendering/meshlet_culling_backend.hpp"
#include "ve_tracy.hpp"

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

	m_resource_manager = std::make_unique<VeResourceManager>(m_ve_device);
	createBuffers();
	m_scene_resources = std::make_unique<SceneResourceManager>(m_ve_device);
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
		m_last_input_actions = m_input_controller.processInput(m_frame_time, m_camera);
		m_ui.visible = m_last_input_actions.ui_visible;
		m_editor->getState().editor_mode = m_last_input_actions.ui_visible;
		if (m_last_input_actions.toggle_performance_ui)
			m_ui.show_performance = !m_ui.show_performance;
		updateCamera(glm::radians(m_ui.fov));

		// Process pending entity deletions at a safe point
		if (m_active_scene) {
			auto& registry = m_active_scene->getRegistry();
			if (registry.hasPendingDeletions()) {
				m_ve_device.getDevice().waitIdle();
				registry.processPendingDeletions();
			}
		}

		// Process editor-driven scene load requests
		processSceneLoadRequest();

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

		// Engine pipeline
		applySettingChanges();
		if (m_editor->beginFrame())
			recreateResolutionDependentSystems(); // extent changed
		selectBackend();
		bool gpu_culling = m_ui.gpu_culling_enabled
			&& m_scene_resources->getGpuSceneManager().hasRegisteredObjects();
		bool hiz_on = m_ui.hiz_occlusion_enabled && m_ui.depth_prepass_enabled && gpu_culling;
		m_active_backend->setHizEnabled(hiz_on);
		// Reload IBL when skybox changes (before frame recording begins)
		size_t skybox_idx = m_skybox_render_system->getCurrentSkyboxIndex();
		if (skybox_idx != m_last_skybox_index) {
			m_last_skybox_index = skybox_idx;
			auto& skyboxes = m_skybox_render_system->getAvailableSkyboxes();
			if (skybox_idx < skyboxes.size())
				m_ibl_system->loadForSkybox(skyboxes[skybox_idx].path);
		}

		VeFrameInfo fi = buildFrameInfo();
		{
			ZoneScopedN("Populate UBO");
			populateUBO(fi);
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
		m_shadow_render_system->subscribeToRegistry(m_active_scene->getRegistry());
		glm::vec4 ambient = m_active_scene->getDefaultAmbient();
		m_ui.ambient_light_color = glm::vec3(ambient);
		m_ui.ambient_light_intensity = ambient.w;

		m_scene_resources->loadScene(m_active_scene->getRegistry(), *m_pbr_render_system);

		// Recreate meshlet culling descriptors (mega buffer was destroyed/rebuilt)
		if (m_meshlet_culling_system) {
			auto& gpu_scene = m_scene_resources->getGpuSceneManager();
			m_meshlet_culling_system->createDescriptorSets(*m_global_pool,
				gpu_scene, m_pbr_render_system->getMegaBuffer());
			m_meshlet_culling_system->createHizDescriptorSets(*m_global_pool,
				gpu_scene, m_pbr_render_system->getMegaBuffer(), *m_hiz_system);
			m_meshlet_culling_system->createShadowDescriptorSets(*m_global_pool,
				gpu_scene, m_pbr_render_system->getMegaBuffer());
		}
	}
}

Registry* VeApplication::getActiveRegistry() {
	return m_active_scene ? &m_active_scene->getRegistry() : nullptr;
}

void VeApplication::unloadScene() {
	if (!m_active_scene)
		return;
	m_scene_resources->unload(*m_pbr_render_system, *m_gpu_culling_system);
	m_active_scene.reset();
}

void VeApplication::registerScene(const std::string& name,
								   std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory) {
	m_scene_entries.push_back({name, std::move(factory)});
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
				auto scene = m_scene_entries[static_cast<size_t>(idx)].factory(ctx);
				setActiveScene(std::move(scene));
				m_loaded_scene_index = idx;
				m_current_scene_index = idx;
			}
			break;
		}
		case SceneLoadRequest::Type::LOAD_GLTF_PATH: {
			auto scene = std::make_unique<GltfScene>(ctx, m_pending_load.gltf_path);
			setActiveScene(std::move(scene));
			m_loaded_scene_index = -1;
			m_current_scene_index = -1;
			break;
		}
		case SceneLoadRequest::Type::NEW_EMPTY: {
			auto scene = std::make_unique<GltfScene>(ctx);
			setActiveScene(std::move(scene));
			m_loaded_scene_index = -1;
			m_current_scene_index = -1;
			break;
		}
		case SceneLoadRequest::Type::ADD_MODEL: {
			if (m_active_scene) {
				m_active_scene->addModel(m_pending_load.gltf_path);
				m_scene_resources->rebuildForModelAdd(
					m_active_scene->getRegistry(), *m_pbr_render_system);
				// Recreate meshlet culling descriptors (mega buffer was rebuilt)
				if (m_meshlet_culling_system) {
					auto& gpu_scene = m_scene_resources->getGpuSceneManager();
					m_meshlet_culling_system->createDescriptorSets(*m_global_pool,
						gpu_scene, m_pbr_render_system->getMegaBuffer());
					m_meshlet_culling_system->createHizDescriptorSets(*m_global_pool,
						gpu_scene, m_pbr_render_system->getMegaBuffer(), *m_hiz_system);
					m_meshlet_culling_system->createShadowDescriptorSets(*m_global_pool,
						gpu_scene, m_pbr_render_system->getMegaBuffer());
				}
			}
			break;
		}
		default:
			break;
	}
	m_pending_load.type = SceneLoadRequest::Type::NONE;
}

// ─── Frame Info Construction ─────────────────────────────────────────────────

void VeApplication::selectBackend() {
	auto& gpu_scene = m_scene_resources->getGpuSceneManager();
	bool gpu_ok = m_ui.gpu_culling_enabled && gpu_scene.hasRegisteredObjects();
	if (gpu_ok && m_ui.meshlet_culling_enabled
		&& m_meshlet_culling_system
		&& m_pbr_render_system->getMegaBuffer().hasMeshletData())
		m_active_backend = m_meshlet_backend.get();
	else if (gpu_ok)
		m_active_backend = m_gpu_backend.get();
	else
		m_active_backend = m_cpu_backend.get();

	if (m_meshlet_backend)
		m_meshlet_backend->setGpuShadowFallback(m_ui.meshlet_gpu_shadow_fallback);
}

VeFrameInfo VeApplication::buildFrameInfo() {
	auto& command_buffer = m_ve_renderer.getCurrentCommandBuffer();
	auto& compute_command_buffer = m_ve_renderer.getCurrentComputeCommandBuffer();
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
		.csm_data = {},
		.shadow_atlas_regions = m_shadow_render_system->getAtlasRegions().data(),
		.shadow_atlas_width = m_shadow_render_system->getAtlasWidth(),
		.shadow_atlas_height = m_shadow_render_system->getAtlasHeight(),
		.gpu_culling_active = gpu_culling_active,
		.meshlet_culling_active = meshlet_active,
		.selected_entity = m_editor->getState().selected_entity,
		.cpu_global_descriptor_set = &m_global_descriptor_sets[current_frame],
	};

	fi.ibl_descriptor_set = &m_ibl_system->getOutputDescriptorSet(current_frame);

	return fi;
}

// ─── Setting Change Detection ────────────────────────────────────────────────

void VeApplication::applySettingChanges() {
	m_ve_renderer.getProfiler().setGpuProfilingEnabled(m_ui.gpu_profiling);
	auto extent = m_ve_renderer.getExtent();

	// Topology change (driven by GraphicsPanel)
	if (m_ui.topology != m_last_topology) {
		m_last_topology = m_ui.topology;
		m_pbr_render_system->setTopology(m_ui.topology);
		m_ve_renderer.setSwapChainNeedsRecreation();
	}

	// Check if any setting change requires GPU idle (pipeline recreation or resource destruction)
	bool needs_idle = (m_ui.pcf_samples != m_pcf_samples || m_ui.pcss_filter_samples != m_pcss_filter_samples)
		|| (m_ui.shadow_mask_half_res != m_shadow_mask_half_res)
		|| (m_ui.gtao_half_res != m_gtao_half_res);
	if (needs_idle)
		m_ve_device.getDevice().waitIdle();

	// Recreate shadow pipelines if sample counts changed
	if (m_ui.pcf_samples != m_pcf_samples || m_ui.pcss_filter_samples != m_pcss_filter_samples) {
		m_pcf_samples = m_ui.pcf_samples;
		m_pcss_filter_samples = m_ui.pcss_filter_samples;
		m_pbr_render_system->setShadowSamples(static_cast<uint32_t>(m_pcf_samples), static_cast<uint32_t>(m_pcss_filter_samples));
		m_shadow_mask_system->setShadowSamples(static_cast<uint32_t>(m_pcf_samples), static_cast<uint32_t>(m_pcss_filter_samples));
	}

	// Shadow mask half-res toggle
	if (m_ui.shadow_mask_half_res != m_shadow_mask_half_res) {
		m_shadow_mask_half_res = m_ui.shadow_mask_half_res;
		m_shadow_mask_system->recreate(*m_global_pool, halveExtent(extent, m_shadow_mask_half_res),
			extent,
			m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage());
	}

	// GTAO parameters and half-res toggle
	m_gtao_system->setRadius(m_ui.gtao_radius);
	m_gtao_system->setIntensity(m_ui.gtao_intensity);
	if (m_ui.gtao_half_res != m_gtao_half_res) {
		m_gtao_half_res = m_ui.gtao_half_res;
		m_gtao_system->recreate(*m_global_pool, halveExtent(extent, m_gtao_half_res),
			extent,
			m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage());
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
	ubo.ibl_intensity = (m_ui.ibl_enabled && m_ibl_system->isAvailable()) ? m_ui.ibl_intensity : 0.0f;
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
	m_cluster_light_system->setEnabled(m_ui.cluster_enabled && cluster_light_count > 0);

	// Record compute start timestamp
	m_ve_renderer.getProfiler().beginGpuTimer(fi.compute_command_buffer, ProfileTimer::COMPUTE_TOTAL);

	// Record compute queue commands
	{
		TracyVkZone(m_ve_renderer.getTracyComputeCtx(), *fi.compute_command_buffer, "Compute");
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

	auto& profiler = m_ve_renderer.getProfiler();
	auto& gpu_scene = m_scene_resources->getGpuSceneManager();
	auto& material_mgr = m_scene_resources->getMaterialManager();
	auto& bindless_set = m_scene_resources->getBindlessRegistry().getDescriptorSet();
	m_pbr_render_system->setDepthPrePassActive(m_ui.depth_prepass_enabled);
	material_mgr.flushToDevice(command_buffer);

	{
		TracyVkZone(tracy_gfx, *command_buffer, "Culling");
		if (fi.gpu_culling_active)
			profiler.beginGpuTimer(command_buffer, ProfileTimer::CULLING);
		else
			profiler.beginCpuTimer(ProfileTimer::CULLING);
		m_active_backend->cull(fi, gpu_scene);
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
		m_active_backend->renderDepthPrePass(fi, m_pbr_render_system->getMegaBuffer(),
			*m_depth_prepass_system);
		m_ve_renderer.endDepthPrePass(command_buffer);
		profiler.endGpuTimer(command_buffer, ProfileTimer::DEPTH_PREPASS);
		profiler.endCpuTimer(ProfileTimer::DEPTH_PREPASS);
	}

	// Shadow pass
	if (m_ui.shadow_mode != ShadowMode::DISABLED) {
		ZoneScopedN("Shadow Maps");
		TracyVkZone(tracy_gfx, *command_buffer, "Shadow Maps");
		profiler.beginCpuTimer(ProfileTimer::SHADOW_MAPS);
		profiler.beginGpuTimer(command_buffer, ProfileTimer::SHADOW_MAPS);
		m_active_backend->renderShadows(fi, *m_shadow_render_system,
			m_pbr_render_system->getMegaBuffer(), gpu_scene);
		profiler.endGpuTimer(command_buffer, ProfileTimer::SHADOW_MAPS);
		profiler.endCpuTimer(ProfileTimer::SHADOW_MAPS);
	}

	// Depth read-only consumers: Hi-Z, Shadow Mask, GTAO
	bool any_depth_consumer = m_ui.depth_prepass_enabled
		&& (hiz_active || fi.shadow_mask_active || gtao_active);

	if (any_depth_consumer) {
		// Single barrier: depth attachment -> read-only
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

		if (fi.shadow_mask_active) {
			TracyVkZone(tracy_gfx, *command_buffer, "Shadow Mask");
			m_shadow_mask_system->dispatch(fi);
			fi.shadow_mask_descriptor_set = &m_shadow_mask_system->getOutputDescriptorSet(fi.current_frame);
		}

		if (gtao_active) {
			ZoneScopedN("GTAO");
			TracyVkZone(tracy_gfx, *command_buffer, "GTAO");
			profiler.beginCpuTimer(ProfileTimer::GTAO);
			profiler.beginGpuTimer(command_buffer, ProfileTimer::GTAO);
			m_gtao_system->dispatch(fi);
			profiler.endGpuTimer(command_buffer, ProfileTimer::GTAO);
			profiler.endCpuTimer(ProfileTimer::GTAO);
			fi.ao_descriptor_set = &m_gtao_system->getOutputDescriptorSet(fi.current_frame);
		}

		if (hiz_active) {
			TracyVkZone(tracy_gfx, *command_buffer, "Hi-Z Build");
			m_hiz_system->generate(command_buffer, fi.current_frame);
		}

		// Single barrier: depth read-only -> attachment
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

	// Set default descriptor sets for inactive passes
	if (!fi.shadow_mask_active)
		fi.shadow_mask_descriptor_set = &m_shadow_mask_system->getDummyOutputDescriptorSet();
	if (!gtao_active)
		fi.ao_descriptor_set = &m_gtao_system->getDummyOutputDescriptorSet();

	// Scene render
	{
		ZoneScopedN("Scene Render");
		TracyVkZone(tracy_gfx, *command_buffer, "Scene Render");
		profiler.beginCpuTimer(ProfileTimer::SCENE_RENDER);
		profiler.beginGpuTimer(command_buffer, ProfileTimer::SCENE_RENDER);
		m_ve_renderer.beginSceneRender(command_buffer, m_ui.depth_prepass_enabled);
		m_active_backend->renderOpaque(fi, *m_pbr_render_system, bindless_set);
		m_skybox_render_system->render(fi);
		if (!fi.gpu_culling_active)
			m_pbr_render_system->renderTransparent(fi, bindless_set);
		m_particle_system->render(fi);
		if (m_ui.show_axes)
			m_axes_render_system->render(fi);
		if (m_ui.show_aabb_debug)
			m_aabb_debug_render_system->render(fi);
		m_light_system->render(fi);
		m_fireworks_system->render(fi);
		m_ve_renderer.endSceneRender(command_buffer);
		profiler.endGpuTimer(command_buffer, ProfileTimer::SCENE_RENDER);
		profiler.endCpuTimer(ProfileTimer::SCENE_RENDER);
	}

	// Transparency (CPU sort, WBOIT, or no-op depending on backend)
	m_active_backend->renderTransparency(fi, *m_pbr_render_system, bindless_set,
		gpu_scene, m_ve_renderer);

	// Selection outline: mask + JFA
	auto& editor_state = m_editor->getState();
	bool outline_active = editor_state.outline_enabled && !fi.selected_entity.isNull();
	if (outline_active) {
		m_outline_system->renderMask(fi, *fi.registry, fi.selected_entity);
		if (m_outline_system->hasOutline())
			m_outline_system->dispatchJFA(fi, editor_state.outline_width);
	}

	// Bloom
	if (m_ui.bloom_enabled) {
		ZoneScopedN("Bloom");
		TracyVkZone(tracy_gfx, *command_buffer, "Bloom");
		profiler.beginCpuTimer(ProfileTimer::BLOOM);
		profiler.beginGpuTimer(command_buffer, ProfileTimer::BLOOM);
		m_bloom_system->render(command_buffer);
		profiler.endGpuTimer(command_buffer, ProfileTimer::BLOOM);
		profiler.endCpuTimer(ProfileTimer::BLOOM);
	}

	// Post-processing + outline composite
	{
		ZoneScopedN("Post Process");
		TracyVkZone(tracy_gfx, *command_buffer, "Post Process");
		profiler.beginCpuTimer(ProfileTimer::POST_PROCESS);
		profiler.beginGpuTimer(command_buffer, ProfileTimer::POST_PROCESS);
		m_ve_renderer.beginPostProcessRender(command_buffer, editor_mode);
		m_post_process_system->render(command_buffer, fi.post_process_push);
		if (outline_active && m_outline_system->hasOutline())
			m_outline_system->composite(command_buffer, fi.current_frame,
				editor_state.outline_width, editor_state.outline_color);
		m_ve_renderer.endPostProcessRender(command_buffer, editor_mode);
		profiler.endGpuTimer(command_buffer, ProfileTimer::POST_PROCESS);
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

	const auto& results = profiler.getResults();
	m_ui.stats.fence_wait = results.fence_wait_ms;
	m_ui.stats.acquire_wait = results.acquire_wait_ms;
	m_ui.stats.cpu_time = results.cpu(ProfileTimer::FRAME_TOTAL) - results.fence_wait_ms - results.acquire_wait_ms;
	m_ui.stats.gpu_time = results.gpu(ProfileTimer::FRAME_TOTAL);
	m_ui.stats.compute_gpu_time = results.gpu(ProfileTimer::COMPUTE_TOTAL);
	m_ui.stats.gpu_overlap = results.gpu_overlap;

	// Per-system GPU breakdown
	m_ui.stats.gpu_shadow_maps = results.gpu(ProfileTimer::SHADOW_MAPS);
	m_ui.stats.gpu_depth_prepass = results.gpu(ProfileTimer::DEPTH_PREPASS);
	m_ui.stats.gpu_gtao = results.gpu(ProfileTimer::GTAO);
	m_ui.stats.gpu_scene_render = results.gpu(ProfileTimer::SCENE_RENDER);
	m_ui.stats.gpu_bloom = results.gpu(ProfileTimer::BLOOM);
	m_ui.stats.gpu_post_process = results.gpu(ProfileTimer::POST_PROCESS);

	// Per-system CPU breakdown
	m_ui.stats.cpu_culling = results.cpu(ProfileTimer::CULLING);
	m_ui.stats.cpu_shadow_maps = results.cpu(ProfileTimer::SHADOW_MAPS);
	m_ui.stats.cpu_depth_prepass = results.cpu(ProfileTimer::DEPTH_PREPASS);
	m_ui.stats.cpu_gtao = results.cpu(ProfileTimer::GTAO);
	m_ui.stats.cpu_scene_render = results.cpu(ProfileTimer::SCENE_RENDER);
	m_ui.stats.cpu_bloom = results.cpu(ProfileTimer::BLOOM);
	m_ui.stats.cpu_post_process = results.cpu(ProfileTimer::POST_PROCESS);
}

// ─── Swap Chain Recreation ───────────────────────────────────────────────────

void VeApplication::onSwapChainRecreated() {
	recreatePipelines();
	m_editor->onSwapChainRecreated();
}

void VeApplication::recreatePipelines() {
	// Precondition: device must be idle
	m_ve_device.assertDeviceIdle();
	auto offscreen_format = m_ve_renderer.getOffscreenImageFormat();
	auto sample_count = m_ve_renderer.getSampleCount();

	m_light_system->recreatePipeline(offscreen_format, sample_count);
	m_pbr_render_system->recreatePipeline(offscreen_format, sample_count);
	m_aabb_debug_render_system->recreatePipeline(offscreen_format, sample_count);
	m_axes_render_system->recreatePipeline(offscreen_format, sample_count);
	m_skybox_render_system->recreatePipeline(offscreen_format, sample_count);
	m_particle_system->recreatePipeline(offscreen_format, sample_count);
	m_fireworks_system->recreatePipeline(offscreen_format, sample_count);
	m_depth_prepass_system->recreatePipeline(sample_count);

	recreateResolutionDependentSystems();

	m_imgui_layer->recreatePipeline();
}

// TODO: make cleaner
void VeApplication::recreateResolutionDependentSystems() {
	auto extent = m_ve_renderer.getExtent();
	auto color_format = m_ve_renderer.getSwapChainImageFormat();

	m_bloom_system->recreateResources(extent, m_ve_renderer.getResolveTargetImageView());
	m_cluster_light_system->recreate(*m_global_pool, extent);

	m_shadow_mask_system->recreate(*m_global_pool, halveExtent(extent, m_shadow_mask_half_res),
		extent,
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage());

	m_gtao_system->recreate(*m_global_pool, halveExtent(extent, m_gtao_half_res),
		extent,
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage());

	m_hiz_system->recreate(*m_global_pool, extent,
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage());
	m_gpu_culling_system->createHizDescriptorSets(*m_global_pool, m_scene_resources->getGpuSceneManager(), *m_hiz_system);
	m_gpu_culling_system->createShadowHizDescriptorSets(*m_global_pool, m_scene_resources->getGpuSceneManager(), *m_hiz_system);
	if (m_meshlet_culling_system) {
		m_meshlet_culling_system->createHizDescriptorSets(*m_global_pool,
			m_scene_resources->getGpuSceneManager(), m_pbr_render_system->getMegaBuffer(), *m_hiz_system);
	}

	m_pbr_render_system->recreateWboit(
		m_ve_renderer.getWboitAccumImageView(),
		m_ve_renderer.getWboitRevealageImageView(),
		m_ve_renderer.getOffscreenImageFormat());

	m_post_process_system->recreatePipeline(color_format, m_ve_renderer.getResolveTargetImageView(), m_bloom_system->getBloomTexture());
	m_outline_system->recreate(*m_global_pool, extent, color_format);
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

	m_culling_system = std::make_unique<CullingSystem>(m_camera);

	m_shadow_render_system = std::make_unique<ShadowRenderSystem>(
		m_ve_device, *m_global_pool,
		m_material_set_layout->getDescriptorSetLayout(),
		shader("shadow_shader.spv")
	);

	m_depth_prepass_system = std::make_unique<DepthPrePassSystem>(
		m_ve_device, m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getSampleCount(), shader("depth_prepass_shader.spv")
	);

	m_shadow_mask_system = std::make_unique<ShadowMaskSystem>(
		m_ve_device, *m_global_pool, *m_resource_manager,
		m_global_set_layout->getDescriptorSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_config.shaders_dir, halveExtent(m_ve_renderer.getExtent(), m_ui.shadow_mask_half_res),
		m_ve_renderer.getExtent(),
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage()
	);
	m_shadow_mask_half_res = m_ui.shadow_mask_half_res;

	m_gtao_system = std::make_unique<GtaoSystem>(
		m_ve_device, *m_global_pool, *m_resource_manager,
		m_global_set_layout->getDescriptorSetLayout(),
		m_config.shaders_dir, halveExtent(m_ve_renderer.getExtent(), m_ui.gtao_half_res),
		m_ve_renderer.getExtent(),
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage()
	);
	m_gtao_half_res = m_ui.gtao_half_res;

	m_cluster_light_system = std::make_unique<ClusterLightSystem>(
		m_ve_device, *m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		shader("cluster_assign_comp.spv"), m_ve_renderer.getExtent()
	);

	m_ibl_system = std::make_unique<IblSystem>(
		m_ve_device, *m_global_pool, *m_resource_manager,
		m_config.skybox_dir / "brdf_lut.ktx"
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
		shader("pbr_shader.spv")
	);
	m_pbr_render_system->initWboit(
		m_ve_renderer.getWboitAccumImageView(),
		m_ve_renderer.getWboitRevealageImageView(),
		m_ve_renderer.getOffscreenImageFormat());

	m_aabb_debug_render_system = std::make_unique<AabbDebugRenderSystem>(
		m_ve_device, m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("axes_shader.spv")
	);

	m_axes_render_system = std::make_unique<AxesRenderSystem>(
		m_ve_device, *m_resource_manager,
		m_global_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("axes_shader.spv")
	);

	m_light_system = std::make_unique<LightSystem>(
		m_ve_device,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("light_billboard_shader.spv")
	);

	m_particle_system = std::make_unique<ParticleSystem>(
		m_ve_device, m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		50000, glm::vec3{0.0f, -300.0f, 50.0f},
		shader("particle_compute.spv")
	);

	m_fireworks_system = std::make_unique<FireworksSystem>(
		m_ve_device, m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_material_set_layout->getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("particle_compute.spv")
	);

	m_skybox_render_system = std::make_unique<SkyboxRenderSystem>(
		m_ve_device, *m_resource_manager, *m_global_pool, *m_material_set_layout,
		m_global_set_layout->getDescriptorSetLayout(),
		m_config.skybox_dir, shader("skybox_shader.spv"), m_config.cube_model,
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount()
	);

	// Load IBL for the initial skybox
	if (!m_skybox_render_system->getAvailableSkyboxes().empty()) {
		m_ibl_system->loadForSkybox(m_skybox_render_system->getAvailableSkyboxes()[0].path);
		m_last_skybox_index = 0;
	}

	m_bloom_system = std::make_unique<BloomSystem>(
		m_ve_device, m_ve_renderer.getExtent(),
		m_ve_renderer.getResolveTargetImageView(),
		shader("bloom_downsample.spv"), shader("bloom_upsample.spv")
	);

	m_post_process_system = std::make_unique<PostProcessSystem>(
		m_ve_device, m_ve_renderer.getSwapChainImageFormat(),
		m_ve_renderer.getResolveTargetImageView(),
		m_bloom_system->getBloomTexture(),
		shader("post_process.spv")
	);

	m_outline_system = std::make_unique<OutlineSystem>(
		m_ve_device, *m_global_pool,
		m_global_set_layout->getDescriptorSetLayout(),
		m_config.shaders_dir,
		m_ve_renderer.getExtent(),
		m_ve_renderer.getSwapChainImageFormat()
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
		m_config.shaders_dir);
	m_gpu_culling_system->createHizDescriptorSets(*m_global_pool, gpu_scene, *m_hiz_system);
	m_gpu_culling_system->createShadowHizDescriptorSets(*m_global_pool, gpu_scene, *m_hiz_system);

	// Meshlet culling
	m_meshlet_culling_system = std::make_unique<MeshletCullingSystem>(m_ve_device, m_config.shaders_dir);
	m_meshlet_culling_system->createDescriptorSets(*m_global_pool,
		gpu_scene, m_pbr_render_system->getMegaBuffer());
	m_meshlet_culling_system->createHizDescriptorSets(*m_global_pool,
		gpu_scene, m_pbr_render_system->getMegaBuffer(), *m_hiz_system);
	m_meshlet_culling_system->createGlobalDescriptorSets(*m_global_pool, *m_global_set_layout,
		m_uniform_buffers, m_scene_resources->getMaterialManager().getBuffer());
	m_meshlet_culling_system->createShadowDescriptorSets(*m_global_pool,
		gpu_scene, m_pbr_render_system->getMegaBuffer());
	m_shadow_render_system->createMeshletShadowDescriptorSets(*m_meshlet_culling_system);

	// Culling backends
	m_cpu_backend = std::make_unique<CpuCullingBackend>(
		*m_culling_system, *m_pbr_render_system,
		m_scene_resources->getMaterialManager(), m_ui,
		m_global_descriptor_sets, m_ve_renderer.getThreadPool());
	m_gpu_backend = std::make_unique<GpuCullingBackend>(
		*m_gpu_culling_system, gpu_scene);
	m_meshlet_backend = std::make_unique<MeshletCullingBackend>(
		*m_meshlet_culling_system, *m_gpu_culling_system);
	m_active_backend = m_cpu_backend.get();
}

void VeApplication::initEditor() {
	VE_LOGD("Initialising UI");
	m_imgui_layer = std::make_unique<ImGuiLayer>(m_ve_window, m_ve_device, m_ve_renderer);
	m_imgui_layer->setAppSettingsWindowName(m_config.app_name);
	m_editor = std::make_unique<Editor>(m_ve_renderer, *m_imgui_layer);
	m_editor->setAppUICallback([this]() { renderUI(); });

	// Wire scene registry and systems into editor
	m_editor->setSceneRegistry(&m_scene_entries, &m_current_scene_index, &m_pending_load);
	m_editor->setSkyboxSystem(m_skybox_render_system.get());
	m_editor->setCamera(&m_camera);

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
