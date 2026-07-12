#include "pch.hpp"
#include "rendering/render_pipeline.hpp"

#include "application/ve_engine_config.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"
#include "rendering/debug_draw_system.hpp"
#include "rendering/bloom_system.hpp"
#include "rendering/cluster_light_system.hpp"
#include "rendering/culling/cpu_culling_backend.hpp"
#include "rendering/culling/culling_backend.hpp"
#include "rendering/culling/culling_system.hpp"
#include "rendering/culling/gpu_culling_backend.hpp"
#include "rendering/culling/gpu_culling_system.hpp"
#include "rendering/culling/meshlet_culling_backend.hpp"
#include "rendering/culling/meshlet_culling_system.hpp"
#include "rendering/frame_profiler.hpp"
#include "rendering/geometry_prepass_system.hpp"
#include "rendering/gtao_system.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/ibl_system.hpp"
#include "rendering/light_system.hpp"
#include "rendering/managers/bindless_texture_registry.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "rendering/managers/material_ssbo_manager.hpp"
#include "rendering/managers/scene_resource_manager.hpp"
#include "rendering/outline_system.hpp"
#include "rendering/particle_backend.hpp"
#include "rendering/particle_emitter_system.hpp"
#include "rendering/pbr_render_system.hpp"
#include "rendering/post_process_system.hpp"
#include "rendering/shadow_mask_system.hpp"
#include "rendering/shadow_render_system.hpp"
#include "rendering/skinned_points_render_system.hpp"
#include "rendering/ssr_system.hpp"
#include "rendering/deform_pre_pass.hpp"
#include "rendering/skybox_render_system.hpp"
#include "rendering/ve_frame_info.hpp"
#include "rendering/ve_renderer.hpp"
#include "resources/asset_loading_system.hpp"
#include "resources/ve_material_properties.hpp"
#include "scene/ve_scene.hpp"
#include "ui/editor_state.hpp"
#include "rendering/render_settings.hpp"
#include "rendering/frame_stats.hpp"
#include "rendering/render_resources.hpp"
#include "rendering/settings_watcher.hpp"
#include "ve_tracy.hpp"
#include "vulkan/ve_debug_utils.hpp"
#include "utils/ve_log.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_device.hpp"

namespace ve {

static vk::Extent2D halveExtent(vk::Extent2D e, bool half) {
	if (!half)
		return e;
	return {std::max(1u, e.width / 2), std::max(1u, e.height / 2)};
}

RenderPipeline::RenderPipeline(VeDevice& device,
                               VeRenderer& renderer,
                               VeResourceManager& resource_manager,
                               RenderResources& resources,
                               EventBus& event_bus,
                               const EngineConfig& config)
	: m_ve_device(device),
	  m_ve_renderer(renderer),
	  m_resource_manager(resource_manager),
	  m_resources(resources),
	  m_event_bus(event_bus),
	  m_config(config) {
	m_scene_resources = std::make_unique<SceneResourceManager>(m_ve_device, m_event_bus, m_resource_manager);
	m_scene_resources->subscribeToEvents(m_event_bus);
	createPerFrameResources(m_scene_resources->getMaterialManager().getBuffer());
	initRenderSystems();
	m_settings_watcher = std::make_unique<SettingsWatcher>(
		m_ve_device, m_ve_renderer, m_event_bus, m_settings, m_resources);

	m_scene_loaded_sub = m_event_bus.subscribe<SceneLoadedEvent>([this](const SceneLoadedEvent& e) {
		if (!e.scene)
			return;
		m_ssr_system->invalidateHistory();
		// Configure particle backend
		uint32_t requested = e.scene->getParticleCapacity();
		uint32_t target = (requested > 0)
			? std::min(requested, m_config.max_particle_capacity)
			: m_config.default_particle_capacity;
		if (target != m_particle_backend->getCapacity())
			m_particle_backend->setCapacity(target);
		m_particle_backend->scheduleRestart();
		m_particle_backend->setEnabled(true);
	});

	m_swap_chain_recreated_sub = m_event_bus.subscribe<SwapChainRecreatedEvent>([this](const SwapChainRecreatedEvent&) {
		m_ve_device.assertDeviceIdle();
		emitSwapChainRecreatedEvents();
	});
	m_viewport_resized_sub = m_event_bus.subscribe<ViewportResizedEvent>([this](const ViewportResizedEvent&) {
		emitResolutionChangedEvent();
	});
}

RenderPipeline::~RenderPipeline() {
	m_event_bus.unsubscribe<SceneLoadedEvent>(m_scene_loaded_sub);
	m_event_bus.unsubscribe<SwapChainRecreatedEvent>(m_swap_chain_recreated_sub);
	m_event_bus.unsubscribe<ViewportResizedEvent>(m_viewport_resized_sub);
}

void RenderPipeline::createPerFrameResources(const VeBuffer& material_ssbo) {
	VE_LOGD("Creating per-frame resources");
	vk::DeviceSize ubo_size = sizeof(UniformBufferObject);
	assert(ubo_size > 0 && "Uniform buffer size is zero");
	assert(ubo_size <= m_ve_device.getDeviceProperties().limits.maxUniformBufferRange && "Uniform buffer size exceeds maximum limit");

	m_uniform_buffers.reserve(MAX_FRAMES_IN_FLIGHT);
	m_instance_buffers.reserve(MAX_FRAMES_IN_FLIGHT);
	m_global_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);

	auto material_ssbo_info = material_ssbo.getDescriptorInfo();
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		auto ub = std::make_unique<VeBuffer>(
			m_ve_device, ubo_size, 1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment);
		ub->map();
		auto ib = std::make_unique<VeBuffer>(
			m_ve_device, sizeof(InstanceData), INITIAL_INSTANCE_CAPACITY,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		ib->map();

		auto ubo_info = ub->getDescriptorInfo();
		auto inst_info = ib->getDescriptorInfo();
		vk::raii::DescriptorSet set{nullptr};
		VeDescriptorWriter(m_resources.globalSetLayout(), *m_resources.pool())
			.writeBuffer(0, &ubo_info)
			.writeBuffer(1, &inst_info)
			.writeBuffer(2, &material_ssbo_info)
			.build(set);

		m_uniform_buffers.push_back(std::move(ub));
		m_instance_buffers.push_back(std::move(ib));
		m_global_descriptor_sets.push_back(std::move(set));
	}
}

void RenderPipeline::initRenderSystems() {
	VE_LOGD("Initialising systems");
	auto shader = [this](const char* name) { return m_config.shaders_dir / name; };

	m_culling_system = std::make_unique<CullingSystem>();

	m_shadow_render_system = std::make_unique<ShadowRenderSystem>(
		m_ve_device, *m_resources.pool(),
		m_resources.materialSetLayout().getDescriptorSetLayout(),
		shader("shadow_shader.spv"),
		m_event_bus
	);
	m_shadow_render_system->setTracyContext(m_ve_renderer.getTracyGraphicsCtx());

	m_geometry_prepass_system = std::make_unique<GeometryPrePassSystem>(
		m_ve_device, m_resources.globalSetLayout().getDescriptorSetLayout(),
		m_scene_resources->getBindlessRegistry().getSetLayout(),
		m_ve_renderer.getSampleCount(), m_ve_renderer.getOffscreenImageFormat(),
		shader("geometry_prepass_shader.spv"),
		m_event_bus
	);

	m_shadow_mask_system = std::make_unique<ShadowMaskSystem>(
		m_ve_device, *m_resources.pool(), m_resource_manager,
		m_resources.globalSetLayout().getDescriptorSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_config.shaders_dir, halveExtent(m_ve_renderer.getExtent(), m_settings.shadow_mask_half_res),
		m_ve_renderer.getExtent(),
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage(),
		m_ve_renderer.getResolvedNormalRoughnessImageView(),
		m_event_bus
	);

	m_gtao_system = std::make_unique<GtaoSystem>(
		m_ve_device, *m_resources.pool(), m_resource_manager,
		m_resources.globalSetLayout().getDescriptorSetLayout(),
		m_config.shaders_dir, halveExtent(m_ve_renderer.getExtent(), m_settings.gtao_half_res),
		m_ve_renderer.getExtent(),
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage(),
		m_ve_renderer.getResolvedNormalRoughnessImageView(),
		m_event_bus
	);

	m_ssr_system = std::make_unique<SsrSystem>(
		m_ve_device, *m_resources.pool(),
		m_resources.globalSetLayout().getDescriptorSetLayout(),
		m_config.shaders_dir,
		halveExtent(m_ve_renderer.getExtent(), m_settings.ssr_half_res),
		m_ve_renderer.getExtent(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getResolvedDepthImageView(),
		m_ve_renderer.getResolvedNormalRoughnessImageView(),
		m_event_bus
	);

	m_cluster_light_system = std::make_unique<ClusterLightSystem>(
		m_ve_device, *m_resources.pool(),
		m_resources.globalSetLayout().getDescriptorSetLayout(),
		shader("cluster_assign_comp.spv"), m_ve_renderer.getExtent(),
		m_event_bus
	);

	// IblSystem subscribes to SkyboxChangedEvent (must be constructed before SkyboxRenderSystem)
	m_ibl_system = std::make_unique<IblSystem>(
		m_ve_device, *m_resources.pool(), m_resource_manager,
		m_config.skybox_dir / "brdf_lut.ktx",
		m_event_bus
	);

	m_pbr_render_system = std::make_unique<PbrRenderSystem>(
		m_ve_device,
		m_resources.globalSetLayout().getDescriptorSetLayout(),
		m_scene_resources->getBindlessRegistry().getSetLayout(),
		m_shadow_render_system->getShadowSetLayout(),
		m_shadow_mask_system->getShadowMaskSetLayout(),
		m_cluster_light_system->getOutputSetLayout(),
		m_gtao_system->getAoSetLayout(),
		m_ibl_system->getIblSetLayout(),
		m_ssr_system->getSsrSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(),
		m_ve_renderer.getSampleCount(),
		shader("pbr_shader.spv"),
		m_event_bus,
		m_scene_resources->getMegaBuffer()
	);
	m_pbr_render_system->initWboit(
		m_ve_renderer.getWboitAccumImageView(),
		m_ve_renderer.getWboitRevealageImageView(),
		m_ve_renderer.getOffscreenImageFormat());

	m_debug_draw_system = std::make_unique<DebugDrawSystem>(
		m_ve_device, m_resource_manager,
		m_resources.globalSetLayout().getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("debug_line_shader.spv"), shader("axes_shader.spv"),
		m_event_bus
	);

	auto light_billboard_texture = m_config.light_billboard_texture.empty()
		? m_resources.defaultParticleTexture()
		: m_resource_manager.load<VeTexture>(m_config.light_billboard_texture.lexically_normal().generic_string());
	m_light_system = std::make_unique<LightSystem>(
		m_ve_device,
		*m_resources.pool(),
		m_resources.globalSetLayout().getDescriptorSetLayout(),
		std::move(light_billboard_texture),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("light_billboard_shader.spv"),
		m_event_bus
	);

	m_particle_backend = std::make_unique<ParticleBackend>(ParticleBackendCreateInfo{
		.device = m_ve_device,
		.descriptor_pool = m_resources.pool(),
		.global_set_layout = m_resources.globalSetLayout().getDescriptorSetLayout(),
		// Safe default for every unallocated bindless atlas slot.
		.default_atlas = m_resources.defaultParticleTexture(),
		.color_format = m_ve_renderer.getOffscreenImageFormat(),
		.sample_count = m_ve_renderer.getSampleCount(),
		.capacity = m_config.default_particle_capacity,
		.shader_path = shader("particle_shader.spv"),
		.event_bus = &m_event_bus,
	});

	m_particle_emitter_system = std::make_unique<ParticleEmitterSystem>(*m_particle_backend, m_event_bus);

	m_skybox_render_system = std::make_unique<SkyboxRenderSystem>(
		m_ve_device, m_resource_manager, *m_resources.pool(), m_resources.materialSetLayout(),
		m_resources.globalSetLayout().getDescriptorSetLayout(),
		m_config.skybox_dir, shader("skybox_shader.spv"),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		m_event_bus
	);

	m_bloom_system = std::make_unique<BloomSystem>(
		m_ve_device, m_ve_renderer.getExtent(),
		m_ve_renderer.getResolveTargetImageView(),
		shader("bloom_downsample.spv"), shader("bloom_upsample.spv"),
		m_event_bus
	);

	m_post_process_system = std::make_unique<PostProcessSystem>(
		m_ve_device, m_ve_renderer.getSwapChainImageFormat(),
		m_ve_renderer.getResolveTargetImageView(),
		m_bloom_system->getBloomTexture(),
		shader("post_process.spv"),
		m_event_bus, *m_bloom_system
	);

	m_outline_system = std::make_unique<OutlineSystem>(
		m_ve_device, *m_resources.pool(),
		m_resources.globalSetLayout().getDescriptorSetLayout(),
		m_config.shaders_dir,
		m_ve_renderer.getExtent(),
		m_ve_renderer.getSwapChainImageFormat(),
		m_event_bus
	);

	m_deform_pre_pass = std::make_unique<DeformPrePass>(
		m_ve_device, *m_resources.pool(), shader("deform_comp.spv"), m_event_bus);

	m_skinned_points_render_system = std::make_unique<SkinnedPointsRenderSystem>(
		m_ve_device, m_resources.globalSetLayout().getDescriptorSetLayout(),
		m_ve_renderer.getOffscreenImageFormat(), m_ve_renderer.getSampleCount(),
		shader("skinned_points.spv"), m_event_bus);

	m_cpu_backend = std::make_unique<CpuCullingBackend>(
		*m_culling_system, *m_pbr_render_system,
		m_scene_resources->getMaterialManager(), m_settings,
		m_global_descriptor_sets, m_ve_renderer.getThreadPool());
	m_active_backend = m_cpu_backend.get();

	m_event_bus.subscribe<TopologyChangedEvent>([this](const TopologyChangedEvent&) {
		m_ve_renderer.setSwapChainNeedsRecreation();
	});

	m_services = RenderServices{
		.skybox    = m_skybox_render_system.get(),
		.shadow    = m_shadow_render_system.get(),
		.particles = m_particle_backend.get(),
	};
}

void RenderPipeline::emitSwapChainRecreatedEvents() {
	m_event_bus.emitImmediate(PipelineRecreateEvent{
		.offscreen_format = m_ve_renderer.getOffscreenImageFormat(),
		.sample_count = m_ve_renderer.getSampleCount()
	});
	emitResolutionChangedEvent();
}

void RenderPipeline::emitResolutionChangedEvent() {
	auto extent = m_ve_renderer.getExtent();
	m_event_bus.emitImmediate(ResolutionChangedEvent{
		.pool = *m_resources.pool(),
		.extent = extent,
		.swap_chain_format = m_ve_renderer.getSwapChainImageFormat(),
		.offscreen_format = m_ve_renderer.getOffscreenImageFormat(),
		.resolve_target_view = m_ve_renderer.getResolveTargetImageView(),
		.depth_image_view = m_ve_renderer.getResolvedDepthImageView(),
		.depth_image = m_ve_renderer.getResolvedDepthImage(),
		.normal_roughness_image_view = m_ve_renderer.getResolvedNormalRoughnessImageView(),
		.wboit_accum_view = m_ve_renderer.getWboitAccumImageView(),
		.wboit_revealage_view = m_ve_renderer.getWboitRevealageImageView(),
		.shadow_mask_half_res = m_settings.shadow_mask_half_res,
		.gtao_half_res = m_settings.gtao_half_res,
		.ssr_half_res = m_settings.ssr_half_res
	});
}

void RenderPipeline::pushPerFrameSettings() {
	m_ve_renderer.getProfiler().setGpuProfilingEnabled(m_settings.gpu_profiling);
}

void RenderPipeline::prepareFrame() {
	pushPerFrameSettings();
	m_settings_watcher->tick();
	m_particle_backend->applyPendingResize();
}

void RenderPipeline::renderFrame(VeScene& scene,
                                 const CameraView& camera_view,
                                 const EditorState& editor_state,
                                 float frame_time,
                                 float total_time) {
	selectBackend();

	bool has_objects = m_scene_resources->getGpuSceneManager().hasRegisteredObjects();
	bool any_gpu_culling = has_objects
		&& m_settings.culling_backend != CullingBackendMode::CPU;
	bool hiz_on = m_settings.hiz_occlusion_enabled && m_settings.geometry_prepass_enabled && any_gpu_culling;
	m_active_backend->setHizEnabled(hiz_on);

	m_skybox_render_system->processPendingLoad();

	VeFrameInfo fi = buildFrameInfo(scene, camera_view, editor_state, frame_time, total_time);
	{
		ZoneScopedN("Populate UBO");
		populateUBO(fi);
	}
	{
		ZoneScopedN("Skinning Palette");
		auto& profiler = m_ve_renderer.getProfiler();
		profiler.beginCpuTimer(ProfileTimer::SKINNING);
		m_deform_pre_pass->updatePalette(scene.getRegistry(), fi.current_frame,
			&m_ve_renderer.getThreadPool());
		m_deform_pre_pass->updateDeformedOffsets(
			m_scene_resources->getGpuSceneManager(),
			m_scene_resources->getMegaBuffer(),
			fi.current_frame);
		m_scene_resources->getGpuSceneManager().refreshSkinnedAabbs(scene.getRegistry());
		profiler.endCpuTimer(ProfileTimer::SKINNING);
	}
	dispatchCompute(fi);
	renderFrameBody(fi, editor_state);
	collectStats(fi, scene.getRegistry());
}

void RenderPipeline::ensureHizInfrastructure() {
	if (m_hiz_system)
		return;
	m_hiz_system = std::make_unique<HizSystem>(
		m_ve_device, *m_resources.pool(), m_ve_renderer.getExtent(),
		m_ve_renderer.getResolvedDepthImageView(), m_ve_renderer.getResolvedDepthImage(),
		m_config.shaders_dir, m_event_bus);
}

void RenderPipeline::ensureGpuCullingInfrastructure() {
	if (m_gpu_culling_system)
		return;
	ensureHizInfrastructure();

	auto& gpu_scene = m_scene_resources->getGpuSceneManager();
	auto& skinning = *m_deform_pre_pass;
	m_gpu_culling_system = std::make_unique<GpuCullingSystem>(m_ve_device, m_config.shaders_dir);
	m_gpu_culling_system->createDescriptorSets(*m_resources.pool(), gpu_scene, skinning);
	m_gpu_culling_system->createShadowDescriptorSets(*m_resources.pool(), gpu_scene, skinning);
	if (m_gpu_culling_system->compactionEnabled())
		m_gpu_culling_system->createCompactionDescriptorSets(*m_resources.pool());
	m_gpu_culling_system->createGlobalDescriptorSets(*m_resources.pool(), m_resources.globalSetLayout(),
		m_uniform_buffers, m_scene_resources->getMaterialManager().getBuffer());

	m_shadow_render_system->createGpuShadowDescriptorSets(*m_gpu_culling_system);

	m_gpu_culling_system->createHizDescriptorSets(*m_resources.pool(), gpu_scene, *m_hiz_system, skinning);
	m_gpu_culling_system->createShadowHizDescriptorSets(*m_resources.pool(), gpu_scene, *m_hiz_system, skinning);
	m_gpu_culling_system->subscribeToEvents(m_event_bus, *m_hiz_system, gpu_scene, skinning);

	m_gpu_backend = std::make_unique<GpuCullingBackend>(*m_gpu_culling_system, gpu_scene);

	if (m_meshlet_backend)
		m_meshlet_backend->setGpuCull(m_gpu_culling_system.get());
}

void RenderPipeline::ensureMeshletCullingInfrastructure() {
	if (m_meshlet_culling_system)
		return;
	ensureHizInfrastructure();

	auto& gpu_scene = m_scene_resources->getGpuSceneManager();
	auto& skinning = *m_deform_pre_pass;
	m_meshlet_culling_system = std::make_unique<MeshletCullingSystem>(
		m_ve_device, m_config.shaders_dir, m_event_bus, *m_resources.pool(),
		*m_scene_resources, m_scene_resources->getMegaBuffer(), *m_hiz_system, skinning);
	m_meshlet_culling_system->createDescriptorSets(*m_resources.pool(),
		gpu_scene, m_scene_resources->getMegaBuffer(), skinning);
	m_meshlet_culling_system->createHizDescriptorSets(*m_resources.pool(),
		gpu_scene, m_scene_resources->getMegaBuffer(), *m_hiz_system, skinning);
	m_meshlet_culling_system->createGlobalDescriptorSets(*m_resources.pool(), m_resources.globalSetLayout(),
		m_uniform_buffers, m_scene_resources->getMaterialManager().getBuffer());
	m_meshlet_culling_system->createShadowDescriptorSets(*m_resources.pool(),
		gpu_scene, m_scene_resources->getMegaBuffer(), skinning);
	m_shadow_render_system->createMeshletShadowDescriptorSets(*m_meshlet_culling_system);

	m_meshlet_backend = std::make_unique<MeshletCullingBackend>(
		*m_meshlet_culling_system, m_gpu_culling_system.get(), gpu_scene);
}

void RenderPipeline::tearDownHizInfrastructure() {
	// Hi-Z is shared: only release once both backends are gone.
	if (!m_hiz_system || m_gpu_culling_system || m_meshlet_culling_system)
		return;
	VE_LOGI("Tearing down Hi-Z infrastructure");
	m_ve_renderer.waitIdle();
	m_hiz_system.reset();
}

void RenderPipeline::tearDownMeshletCullingInfrastructure() {
	if (!m_meshlet_culling_system)
		return;
	VE_LOGI("Tearing down meshlet culling infrastructure");
	m_ve_renderer.waitIdle();
	m_shadow_render_system->releaseMeshletShadowDescriptorSets();
	if (m_active_backend == m_meshlet_backend.get())
		m_active_backend = m_cpu_backend.get();
	m_meshlet_backend.reset();
	m_meshlet_culling_system.reset();
	tearDownHizInfrastructure();
}

void RenderPipeline::tearDownGpuCullingInfrastructure() {
	if (!m_gpu_culling_system)
		return;
	VE_LOGI("Tearing down GPU culling infrastructure");
	m_ve_renderer.waitIdle();
	// Detach the GPU system from the meshlet backend before destroying it
	if (m_meshlet_backend)
		m_meshlet_backend->setGpuCull(nullptr);
	m_shadow_render_system->releaseGpuShadowDescriptorSets();
	if (m_active_backend == m_gpu_backend.get())
		m_active_backend = m_cpu_backend.get();
	m_gpu_backend.reset();
	m_gpu_culling_system.reset();
	tearDownHizInfrastructure();
}

void RenderPipeline::selectBackend() {
	static constexpr uint32_t CULLING_BACKEND_TEARDOWN_FRAMES = 60;

	auto& gpu_scene = m_scene_resources->getGpuSceneManager();
	bool has_objects = gpu_scene.hasRegisteredObjects();
	CullingBackendMode mode = m_settings.culling_backend;
	bool meshlet_ok = mode == CullingBackendMode::MESHLET && has_objects
		&& m_scene_resources->getMegaBuffer().hasMeshletData();
	bool gpu_selected = mode == CullingBackendMode::GPU && has_objects;
	bool gpu_for_meshlet_shadows = meshlet_ok && m_settings.meshlet_object_culled_shadows;
	CullingBackend* prev = m_active_backend;

	if (meshlet_ok) {
		if (gpu_for_meshlet_shadows)
			ensureGpuCullingInfrastructure();
		ensureMeshletCullingInfrastructure();
		m_active_backend = m_meshlet_backend.get();
	}
	else if (gpu_selected) {
		ensureGpuCullingInfrastructure();
		m_active_backend = m_gpu_backend.get();
	}
	else {
		m_active_backend = m_cpu_backend.get();
	}

	// Delayed teardown
	if (m_meshlet_culling_system) {
		if (mode == CullingBackendMode::MESHLET)
			m_meshlet_inactive_frames = 0;
		else if (++m_meshlet_inactive_frames >= CULLING_BACKEND_TEARDOWN_FRAMES) {
			tearDownMeshletCullingInfrastructure();
			m_meshlet_inactive_frames = 0;
		}
	}
	if (m_gpu_culling_system) {
		bool gpu_needed = gpu_selected || gpu_for_meshlet_shadows;
		if (gpu_needed)
			m_gpu_inactive_frames = 0;
		else if (++m_gpu_inactive_frames >= CULLING_BACKEND_TEARDOWN_FRAMES) {
			tearDownGpuCullingInfrastructure();
			m_gpu_inactive_frames = 0;
		}
	}

	if (m_active_backend != prev)
		m_event_bus.emitImmediate(CullingBackendChangedEvent{});
}

VeFrameInfo RenderPipeline::buildFrameInfo(VeScene& scene,
                                          const CameraView& camera_view,
                                          const EditorState& editor_state,
                                          float frame_time,
                                          float total_time) {
	uint32_t current_frame = m_ve_renderer.getCurrentFrame();
	auto& command_buffer = m_ve_renderer.getCurrentCommandBuffer();
	auto& compute_command_buffer = m_ve_renderer.getCurrentComputeCommandBuffer();
	auto& depth_compute_command_buffer = m_ve_renderer.getDepthComputeCommandBuffer();

	vk::raii::DescriptorSet& shadow_desc_set = m_shadow_render_system->getShadowDescriptorSet(current_frame);

	int color_space_type = static_cast<int>(m_ve_renderer.getHDRColorMode());

	auto extent = m_ve_renderer.getExtent();
	glm::vec2 texel_size = {1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};

	bool gpu_culling_active = (m_active_backend != m_cpu_backend.get());
	bool meshlet_active = (m_active_backend == m_meshlet_backend.get());

	VeFrameInfo fi = {
		.command_buffer = &command_buffer,
		.compute_command_buffer = compute_command_buffer,
		.depth_compute_command_buffer = &depth_compute_command_buffer,
		.global_descriptor_set = m_active_backend->getGlobalDescriptorSet(current_frame),
		.cubemap_descriptor_set = m_skybox_render_system->getCubemapDescriptorSet(),
		.shadow_descriptor_set = shadow_desc_set,
		.cpu_global_descriptor_set = &m_global_descriptor_sets[current_frame],
		.active_scene = &scene,
		.registry = &scene.getRegistry(),
		.camera_view = camera_view,
		.selected_entities = editor_state.selected_entities,
		.current_frame = current_frame,
		.frame_time = frame_time,
		.total_time = total_time,
		.visible_objects = m_culling_system->getVisibleObjectsRef(),
		.instance_data = static_cast<InstanceData*>(m_instance_buffers[current_frame]->getMappedMemory()),
		.instance_count = 0,
		.instance_capacity = INITIAL_INSTANCE_CAPACITY,
		.shadow_mode = m_settings.shadow_mode,
		.depth_bias_constant = m_settings.depth_bias_constant,
		.depth_bias_slope = m_settings.depth_bias_slope,
		.depth_bias_clamp = m_settings.depth_bias_clamp,
		.shadow_cull_mode = toVkCullMode(m_settings.shadow_cull_mode),
		.csm_data = {},
		.shadow_atlas_regions = m_shadow_render_system->getAtlasRegions().data(),
		.shadow_atlas_width = m_shadow_render_system->getAtlasWidth(),
		.shadow_atlas_height = m_shadow_render_system->getAtlasHeight(),
		.csm_cascade_resolutions = m_shadow_render_system->getCsmCascadeResolutions(),
		.post_process_push = {
			m_settings.blur_radius,
			m_settings.blur_strength,
			m_settings.exposure,
			color_space_type,
			m_settings.bloom_enabled ? m_settings.bloom_strength : 0.0f,
			m_settings.tone_map_mode,
			m_settings.hdr_peak_white,
			0.0f,
			texel_size
		},
		.gpu_culling_active = gpu_culling_active,
		.meshlet_culling_active = meshlet_active,
		.deform_pre_pass = m_deform_pre_pass.get(),
	};

	fi.ibl_descriptor_set = &m_ibl_system->getOutputDescriptorSet(current_frame);

	return fi;
}

void RenderPipeline::populateUBO(VeFrameInfo& fi) {
	auto current_frame = fi.current_frame;
	auto extent = m_ve_renderer.getExtent();

	UniformBufferObject ubo{};
	ubo.render_mode = m_settings.render_mode;
	ubo.shadow_mode = m_settings.shadow_mode;
	ubo.pcss_light_size = m_settings.pcss_light_size;
	ubo.shadow_bias = m_settings.shadow_bias;
	ubo.csm_normal_bias = m_settings.csm_normal_bias;
	ubo.csm_blend_dithered = static_cast<uint32_t>(m_settings.csm_blend_mode);
	ubo.ambient_light_color = glm::vec4(m_settings.ambient_light_color, m_settings.ambient_light_intensity);
	m_stats.ibl_exposure_compensation = m_ibl_system->isAvailable() ? m_ibl_system->getExposureCompensation() : 1.0f;
	if (m_settings.ibl_enabled && m_ibl_system->isAvailable()) {
		float comp = m_settings.ibl_auto_exposure ? m_stats.ibl_exposure_compensation : 1.0f;
		ubo.ibl_diffuse_intensity = m_settings.ibl_diffuse_intensity * comp;
		ubo.ibl_specular_intensity = m_settings.ibl_specular_intensity * comp;
	} else {
		ubo.ibl_diffuse_intensity = 0.0f;
		ubo.ibl_specular_intensity = 0.0f;
	}
	ubo.ibl_min_ambient = m_settings.ibl_min_ambient;
	ubo.prefiltered_mip_levels = m_ibl_system->getPrefilteredMipLevels();
	auto& sh = m_ibl_system->getSHCoefficients();
	std::copy(sh.begin(), sh.end(), ubo.sh_coefficients);

	m_light_system->updateUniformBuffer(fi, ubo);
	m_shadow_render_system->updateUniformBuffer(current_frame, ubo, fi.csm_data);

	bool shadow_mask_active = m_settings.shadow_mask_enabled
		&& m_settings.geometry_prepass_enabled
		&& m_settings.shadow_mode != ShadowMode::DISABLED;
	ubo.screen_size = glm::vec2(static_cast<float>(extent.width), static_cast<float>(extent.height));

	writeUniformBuffer(current_frame, fi.camera_view, ubo);

	fi.shadow_mask_active = shadow_mask_active;

	// Pipeline-variant selectors
	fi.area_lights_active = ubo.num_rect_lights > 0;
	fi.debug_shading = m_settings.render_mode != RenderMode::BRDF_MICROFACET;
}

void RenderPipeline::dispatchCompute(VeFrameInfo& fi) {
	ZoneScopedN("Compute Dispatch");
	auto current_frame = fi.current_frame;
	auto extent = m_ve_renderer.getExtent();
	auto& profiler = m_ve_renderer.getProfiler();

	profiler.beginCpuTimer(ProfileTimer::CLUSTER_LIGHTS);
	uint32_t cluster_light_count = m_cluster_light_system->uploadLightData(fi);
	m_cluster_light_system->setLightCountActive(cluster_light_count > 0);
	profiler.endCpuTimer(ProfileTimer::CLUSTER_LIGHTS);

	profiler.beginGpuTimer(fi.compute_command_buffer, ProfileTimer::COMPUTE_TOTAL);

	{
		ScopedDebugLabel label(fi.compute_command_buffer, "Compute Dispatch", {0.2f, 0.6f, 0.9f, 1.0f});
		TracyVkZone(m_ve_renderer.getTracyComputeCtx(), *fi.compute_command_buffer, "Compute Dispatch");
		{
			ZoneScopedN("Skinning Dispatch");
			ScopedDebugLabel skin_label(fi.compute_command_buffer, "Skinning Dispatch", {0.9f, 0.6f, 0.9f, 1.0f});
			TracyVkZone(m_ve_renderer.getTracyComputeCtx(), *fi.compute_command_buffer, "Skinning Dispatch");
			profiler.beginGpuTimer(fi.compute_command_buffer, ProfileTimer::SKINNING);
			m_deform_pre_pass->dispatch(fi, m_scene_resources->getMegaBuffer());
			profiler.endGpuTimer(fi.compute_command_buffer, ProfileTimer::SKINNING);
		}
		{
			ZoneScopedN("Particle Dispatch");
			ScopedDebugLabel particle_label(fi.compute_command_buffer, "Particle Dispatch", {0.9f, 0.4f, 0.2f, 1.0f});
			TracyVkZone(m_ve_renderer.getTracyComputeCtx(), *fi.compute_command_buffer, "Particle Dispatch");
			profiler.beginCpuTimer(ProfileTimer::PARTICLES);
			profiler.beginGpuTimer(fi.compute_command_buffer, ProfileTimer::PARTICLES);
			if (fi.registry)
				m_particle_emitter_system->tick(*fi.registry, fi.frame_time);
			m_particle_backend->recordComputeCommands(fi);
			profiler.endGpuTimer(fi.compute_command_buffer, ProfileTimer::PARTICLES);
			profiler.endCpuTimer(ProfileTimer::PARTICLES);
		}
		if (m_cluster_light_system->isEnabled()) {
			ZoneScopedN("Cluster Light Dispatch");
			ScopedDebugLabel cluster_label(fi.compute_command_buffer, "Cluster Light Dispatch", {0.4f, 0.7f, 0.5f, 1.0f});
			TracyVkZone(m_ve_renderer.getTracyComputeCtx(), *fi.compute_command_buffer, "Cluster Light Dispatch");
			profiler.beginCpuTimer(ProfileTimer::CLUSTER_LIGHTS);
			profiler.beginGpuTimer(fi.compute_command_buffer, ProfileTimer::CLUSTER_LIGHTS);
			m_cluster_light_system->dispatch(fi, extent);
			profiler.endGpuTimer(fi.compute_command_buffer, ProfileTimer::CLUSTER_LIGHTS);
			profiler.endCpuTimer(ProfileTimer::CLUSTER_LIGHTS);
		}
	}

	m_ve_renderer.submitCompute(fi.compute_command_buffer);

	fi.cluster_descriptor_set = &m_cluster_light_system->getOutputDescriptorSet(current_frame);
}

void RenderPipeline::renderFrameBody(VeFrameInfo& fi, const EditorState& editor_state) {
	ZoneScopedN("Render Frame");
	m_ve_renderer.markSceneFrame();
	auto& command_buffer = fi.cmd();
	[[maybe_unused]] auto tracy_gfx = m_ve_renderer.getTracyGraphicsCtx();
	[[maybe_unused]] auto tracy_compute = m_ve_renderer.getTracyComputeCtx();

	auto& profiler = m_ve_renderer.getProfiler();
	auto& gpu_scene = m_scene_resources->getGpuSceneManager();
	auto& material_mgr = m_scene_resources->getMaterialManager();
	auto& bindless_set = m_scene_resources->getBindlessRegistry().getDescriptorSet();
	if (material_mgr.isDirty())
		material_mgr.flushToDevice(command_buffer);

	{
		ScopedDebugLabel label(command_buffer, "Culling", {0.6f, 0.6f, 0.6f, 1.0f});
		ZoneScopedN("Culling");
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

	bool editor_mode = editor_state.editor_mode;

	bool hiz_active = fi.gpu_culling_active && m_active_backend->isHizEnabled()
		&& m_settings.geometry_prepass_enabled;
	bool gtao_active = m_settings.gtao_enabled && m_settings.geometry_prepass_enabled;
	bool ssr_active = m_settings.ssr_enabled && m_settings.geometry_prepass_enabled;
	bool perspective_cam = fi.camera_view.proj[3][3] == 0.0f;
	bool ssr_trace_active = ssr_active && perspective_cam && m_ssr_system->historyValid();

	if (m_settings.geometry_prepass_enabled) {
		ZoneScopedN("Geometry Prepass");
		TracyVkZone(tracy_gfx, *command_buffer, "Geometry Prepass");
		profiler.beginCpuTimer(ProfileTimer::GEOMETRY_PREPASS);
		profiler.beginGpuTimer(command_buffer, ProfileTimer::GEOMETRY_PREPASS);
		m_ve_renderer.beginGeometryPrePass(command_buffer);
		m_active_backend->renderGeometryPrePass(fi, m_scene_resources->getMegaBuffer(),
			*m_geometry_prepass_system, bindless_set);
		m_ve_renderer.endGeometryPrePass(command_buffer);
		profiler.endGpuTimer(command_buffer, ProfileTimer::GEOMETRY_PREPASS);
		profiler.endCpuTimer(ProfileTimer::GEOMETRY_PREPASS);
	}

	bool shadows_enabled = m_settings.shadow_mode != ShadowMode::DISABLED;
	bool any_depth_consumer = m_settings.geometry_prepass_enabled
		&& (hiz_active || fi.shadow_mask_active || gtao_active || ssr_trace_active);
	bool any_async_consumer = m_ve_device.hasDedicatedComputeQueue()
		&& (gtao_active || hiz_active);

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
		// The depth producer is the MSAA resolve (COLOR_ATTACHMENT_OUTPUT / write)
		// only when MSAA is active; otherwise it is a depth-stencil write.
		const bool msaa_depth = m_ve_renderer.hasResolvedDepth();
		const vk::PipelineStageFlags2 resolve_stage = msaa_depth
			? vk::PipelineStageFlagBits2::eColorAttachmentOutput : vk::PipelineStageFlags2{};
		const vk::AccessFlags2 resolve_access = msaa_depth
			? vk::AccessFlagBits2::eColorAttachmentWrite : vk::AccessFlags2{};
		std::array<vk::ImageMemoryBarrier2, 2> to_read = {
			vk::ImageMemoryBarrier2{
				.srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
					| vk::PipelineStageFlagBits2::eLateFragmentTests
					| resolve_stage,
				.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite
					| resolve_access,
				.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
				.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
				.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_ve_renderer.getResolvedDepthImage(),
				.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
			},
			vk::ImageMemoryBarrier2{
				.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
				.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
				.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
				.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
				.oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
				.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_ve_renderer.getResolvedNormalRoughnessImage(),
				.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
			}
		};
		vk::DependencyInfo dep{
			.imageMemoryBarrierCount = static_cast<uint32_t>(to_read.size()),
			.pImageMemoryBarriers = to_read.data()
		};
		command_buffer.pipelineBarrier2(dep);

		if (any_async_consumer) {
			m_ve_renderer.submitPreSwapGraphics(/*depth_compute_follows=*/true);

			auto& depth_compute_cb = *fi.depth_compute_command_buffer;

			if (gtao_active) {
				ScopedDebugLabel label(depth_compute_cb, "GTAO", {0.2f, 0.7f, 0.7f, 1.0f});
				ZoneScopedN("GTAO");
				TracyVkZone(tracy_compute, *depth_compute_cb, "GTAO (async)");
				profiler.beginCpuTimer(ProfileTimer::GTAO);
				profiler.beginGpuTimer(depth_compute_cb, ProfileTimer::GTAO);
				m_gtao_system->dispatch(fi, depth_compute_cb);
				profiler.endGpuTimer(depth_compute_cb, ProfileTimer::GTAO);
				profiler.endCpuTimer(ProfileTimer::GTAO);
				fi.ao_descriptor_set = &m_gtao_system->getOutputDescriptorSet(fi.current_frame);
			}

			if (hiz_active) {
				ScopedDebugLabel label(depth_compute_cb, "Hi-Z", {0.4f, 0.4f, 0.8f, 1.0f});
				ZoneScopedN("Hi-Z");
				TracyVkZone(tracy_compute, *depth_compute_cb, "Hi-Z (async)");
				profiler.beginCpuTimer(ProfileTimer::HIZ);
				profiler.beginGpuTimer(depth_compute_cb, ProfileTimer::HIZ);
				m_hiz_system->generate(depth_compute_cb, fi.current_frame);
				profiler.endGpuTimer(depth_compute_cb, ProfileTimer::HIZ);
				profiler.endCpuTimer(ProfileTimer::HIZ);
			}

			auto& shadow_cb = m_ve_renderer.getShadowGraphicsCommandBuffer();
			fi.command_buffer = &shadow_cb;

			if (shadows_enabled) {
				ScopedDebugLabel label(shadow_cb, "Shadow Maps", {0.5f, 0.2f, 0.2f, 1.0f});
				ZoneScopedN("Shadow Maps");
				TracyVkZone(tracy_gfx, *shadow_cb, "Shadow Maps");
				profiler.beginCpuTimer(ProfileTimer::SHADOW_MAPS);
				profiler.beginGpuTimer(shadow_cb, ProfileTimer::SHADOW_MAPS);
				m_active_backend->renderShadows(fi, *m_shadow_render_system,
					m_scene_resources->getMegaBuffer(), gpu_scene);
				profiler.endGpuTimer(shadow_cb, ProfileTimer::SHADOW_MAPS);
				profiler.endCpuTimer(ProfileTimer::SHADOW_MAPS);
			}

			if (fi.shadow_mask_active) {
				ScopedDebugLabel label(shadow_cb, "Shadow Mask", {0.5f, 0.3f, 0.5f, 1.0f});
				ZoneScopedN("Shadow Mask");
				TracyVkZone(tracy_gfx, *shadow_cb, "Shadow Mask");
				profiler.beginCpuTimer(ProfileTimer::SHADOW_MASK);
				profiler.beginGpuTimer(shadow_cb, ProfileTimer::SHADOW_MASK);
				m_shadow_mask_system->dispatch(fi);
				profiler.endGpuTimer(shadow_cb, ProfileTimer::SHADOW_MASK);
				profiler.endCpuTimer(ProfileTimer::SHADOW_MASK);
				fi.shadow_mask_descriptor_set = &m_shadow_mask_system->getOutputDescriptorSet(fi.current_frame);
			}

			if (ssr_trace_active)
				recordSsrTrace(fi, shadow_cb);

			m_ve_renderer.submitShadowGraphics(shadow_cb);
			m_ve_renderer.submitDepthCompute(depth_compute_cb);

			auto& swap_cb = m_ve_renderer.getSwapGraphicsCommandBuffer();
			fi.command_buffer = &swap_cb;

			recordDepthConsumerToAttachBarriers(swap_cb);
		} else {
			if (fi.shadow_mask_active) {
				ScopedDebugLabel label(command_buffer, "Shadow Mask", {0.5f, 0.3f, 0.5f, 1.0f});
				ZoneScopedN("Shadow Mask");
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
				ZoneScopedN("Hi-Z");
				TracyVkZone(tracy_gfx, *command_buffer, "Hi-Z");
				profiler.beginCpuTimer(ProfileTimer::HIZ);
				profiler.beginGpuTimer(command_buffer, ProfileTimer::HIZ);
				m_hiz_system->generate(command_buffer, fi.current_frame);
				profiler.endGpuTimer(command_buffer, ProfileTimer::HIZ);
				profiler.endCpuTimer(ProfileTimer::HIZ);
			}

			if (ssr_trace_active)
				recordSsrTrace(fi, command_buffer);
		}
	}

	if (!any_async_consumer && any_depth_consumer)
		recordDepthConsumerToAttachBarriers(command_buffer);

	if (!fi.shadow_mask_active)
		fi.shadow_mask_descriptor_set = &m_shadow_mask_system->getDummyOutputDescriptorSet();
	if (!gtao_active)
		fi.ao_descriptor_set = &m_gtao_system->getDummyOutputDescriptorSet();
	if (!ssr_trace_active)
		fi.ssr_descriptor_set = &m_ssr_system->getDummyOutputDescriptorSet();

	auto& active_cb = fi.cmd();

	if (gtao_active)
		m_gtao_system->acquireForRead(active_cb, fi.current_frame);

	{
		ZoneScopedN("Scene Render");
		TracyVkZone(tracy_gfx, *active_cb, "Scene Render");
		profiler.beginCpuTimer(ProfileTimer::SCENE_RENDER);
		profiler.beginGpuTimer(active_cb, ProfileTimer::SCENE_RENDER);
		m_ve_renderer.beginSceneRender(active_cb, m_settings.geometry_prepass_enabled);
		{
			ZoneScopedN("Opaque");
			TracyVkZone(tracy_gfx, *active_cb, "Opaque");
			m_active_backend->renderOpaque(fi, *m_pbr_render_system, bindless_set);
		}
		{
			ZoneScopedN("Skybox");
			TracyVkZone(tracy_gfx, *active_cb, "Skybox");
			m_skybox_render_system->render(fi);
		}
		if (!fi.gpu_culling_active) {
			ZoneScopedN("Transparent (inline)");
			TracyVkZone(tracy_gfx, *active_cb, "Transparent (inline)");
			m_pbr_render_system->renderTransparent(fi, bindless_set);
		}
		{
			ZoneScopedN("Particles");
			TracyVkZone(tracy_gfx, *active_cb, "Particles");
			m_particle_backend->render(fi);
		}
		if (m_settings.show_axes || m_settings.show_aabb_debug || m_settings.show_skinned_points
			|| m_settings.show_area_lights) {
			ZoneScopedN("Debug Overlays");
			TracyVkZone(tracy_gfx, *active_cb, "Debug Overlays");
			if (m_settings.show_axes)
				m_debug_draw_system->renderAxes(fi);
			if (m_settings.show_aabb_debug)
				m_debug_draw_system->addVisibleAabbs(fi);
			if (m_settings.show_area_lights)
				m_debug_draw_system->addAreaLightGizmos(fi);
			m_debug_draw_system->render(fi);
			if (m_settings.show_skinned_points)
				m_skinned_points_render_system->render(fi, *m_deform_pre_pass,
					m_scene_resources->getMegaBuffer());
		}
		{
			ZoneScopedN("Light Billboards");
			TracyVkZone(tracy_gfx, *active_cb, "Light Billboards");
			m_light_system->render(fi);
		}
		m_ve_renderer.endSceneRender(active_cb);
		profiler.endGpuTimer(active_cb, ProfileTimer::SCENE_RENDER);
		profiler.endCpuTimer(ProfileTimer::SCENE_RENDER);
	}

	{
		ZoneScopedN("Transparent (WBOIT)");
		TracyVkZone(tracy_gfx, *active_cb, "Transparent (WBOIT)");
		m_active_backend->renderTransparency(fi, *m_pbr_render_system, bindless_set,
			gpu_scene, m_ve_renderer);
	}

	if (ssr_active && perspective_cam) {
		ScopedDebugLabel label(active_cb, "SSR History Copy", {0.3f, 0.6f, 1.0f, 1.0f});
		ZoneScopedN("SSR History Copy");
		TracyVkZone(tracy_gfx, *active_cb, "SSR History Copy");
		m_ssr_system->recordHistoryCopy(active_cb, m_ve_renderer.getResolveTargetImage());
	} else
		m_ssr_system->invalidateHistory();

	bool outline_active = editor_state.outline_enabled && !fi.selected_entities.empty();
	if (outline_active) {
		ScopedDebugLabel label(active_cb, "Selection Outline", {1.0f, 0.5f, 0.0f, 1.0f});
		ZoneScopedN("Selection Outline");
		TracyVkZone(tracy_gfx, *active_cb, "Selection Outline");
		profiler.beginCpuTimer(ProfileTimer::OUTLINE);
		profiler.beginGpuTimer(active_cb, ProfileTimer::OUTLINE);
		m_outline_system->renderMask(fi, *fi.registry, fi.selected_entities,
			m_scene_resources->getMegaBuffer());
		if (m_outline_system->hasOutline())
			m_outline_system->dispatchJFA(fi, editor_state.outline_width);
		profiler.endGpuTimer(active_cb, ProfileTimer::OUTLINE);
		profiler.endCpuTimer(ProfileTimer::OUTLINE);
	}

	if (m_settings.bloom_enabled) {
		ScopedDebugLabel label(active_cb, "Bloom", {1.0f, 0.8f, 0.3f, 1.0f});
		ZoneScopedN("Bloom");
		TracyVkZone(tracy_gfx, *active_cb, "Bloom");
		profiler.beginCpuTimer(ProfileTimer::BLOOM);
		profiler.beginGpuTimer(active_cb, ProfileTimer::BLOOM);
		m_bloom_system->render(active_cb);
		profiler.endGpuTimer(active_cb, ProfileTimer::BLOOM);
		profiler.endCpuTimer(ProfileTimer::BLOOM);
	}

	if (!editor_mode && !m_ve_renderer.ensureImageAcquired())
		return;
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

void RenderPipeline::recordSsrTrace(VeFrameInfo& fi, vk::raii::CommandBuffer& cmd) {
	[[maybe_unused]] auto tracy_gfx = m_ve_renderer.getTracyGraphicsCtx();
	auto& profiler = m_ve_renderer.getProfiler();
	ScopedDebugLabel label(cmd, "SSR Trace", {0.3f, 0.6f, 1.0f, 1.0f});
	ZoneScopedN("SSR Trace");
	TracyVkZone(tracy_gfx, *cmd, "SSR Trace");
	profiler.beginCpuTimer(ProfileTimer::SSR);
	profiler.beginGpuTimer(cmd, ProfileTimer::SSR);
	m_ssr_system->dispatch(fi, cmd);
	profiler.endGpuTimer(cmd, ProfileTimer::SSR);
	profiler.endCpuTimer(ProfileTimer::SSR);
	fi.ssr_descriptor_set = &m_ssr_system->getOutputDescriptorSet();
}

void RenderPipeline::recordDepthConsumerToAttachBarriers(vk::raii::CommandBuffer& cmd) {
	// The upcoming depth write is an MSAA resolve (COLOR_ATTACHMENT_OUTPUT / write)
	// only when MSAA is active; otherwise it is a plain depth-stencil write.
	const bool msaa_depth = m_ve_renderer.hasResolvedDepth();
	const vk::PipelineStageFlags2 resolve_stage = msaa_depth
		? vk::PipelineStageFlagBits2::eColorAttachmentOutput : vk::PipelineStageFlags2{};
	const vk::AccessFlags2 resolve_access = msaa_depth
		? vk::AccessFlagBits2::eColorAttachmentWrite : vk::AccessFlags2{};
	std::array<vk::ImageMemoryBarrier2, 2> to_attach = {
		vk::ImageMemoryBarrier2{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
			.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
				| vk::PipelineStageFlagBits2::eLateFragmentTests
				| resolve_stage,
			.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead
				| vk::AccessFlagBits2::eDepthStencilAttachmentWrite
				| resolve_access,
			.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_ve_renderer.getResolvedDepthImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
		},
		vk::ImageMemoryBarrier2{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
			.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead
				| vk::AccessFlagBits2::eColorAttachmentWrite,
			.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_ve_renderer.getResolvedNormalRoughnessImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
		}
	};
	vk::DependencyInfo dep{
		.imageMemoryBarrierCount = static_cast<uint32_t>(to_attach.size()),
		.pImageMemoryBarriers = to_attach.data()
	};
	cmd.pipelineBarrier2(dep);
}

void RenderPipeline::collectStats(const VeFrameInfo& fi, Registry& registry) {
	auto& profiler = m_ve_renderer.getProfiler();

	m_active_backend->collectStats(fi.current_frame, m_stats, registry);
	m_stats.num_point_lights = registry.activePointLightCount();
	m_stats.num_directional_lights = registry.activeDirectionalLightCount();
	m_stats.num_spot_lights = registry.activeSpotLightCount();
	m_stats.num_area_lights = registry.activeAreaLightCount();

	const auto& results = profiler.getResults();
	m_stats.gpu_culling = results.gpu(ProfileTimer::CULLING);
	m_stats.gpu_shadow_maps = results.gpu(ProfileTimer::SHADOW_MAPS);
	m_stats.gpu_geometry_prepass = results.gpu(ProfileTimer::GEOMETRY_PREPASS);
	m_stats.gpu_gtao = results.gpu(ProfileTimer::GTAO);
	m_stats.gpu_scene_render = results.gpu(ProfileTimer::SCENE_RENDER);
	m_stats.gpu_ssr = results.gpu(ProfileTimer::SSR);
	m_stats.gpu_bloom = results.gpu(ProfileTimer::BLOOM);
	m_stats.gpu_post_process = results.gpu(ProfileTimer::POST_PROCESS);
	m_stats.gpu_hiz = results.gpu(ProfileTimer::HIZ);
	m_stats.gpu_shadow_mask = results.gpu(ProfileTimer::SHADOW_MASK);
	m_stats.gpu_outline = results.gpu(ProfileTimer::OUTLINE);
	m_stats.gpu_skinning = results.gpu(ProfileTimer::SKINNING);
	m_stats.gpu_cluster_lights = results.gpu(ProfileTimer::CLUSTER_LIGHTS);
	m_stats.gpu_particles = results.gpu(ProfileTimer::PARTICLES);

	m_stats.cpu_culling = results.cpu(ProfileTimer::CULLING);
	m_stats.cpu_shadow_maps = results.cpu(ProfileTimer::SHADOW_MAPS);
	m_stats.cpu_geometry_prepass = results.cpu(ProfileTimer::GEOMETRY_PREPASS);
	m_stats.cpu_gtao = results.cpu(ProfileTimer::GTAO);
	m_stats.cpu_scene_render = results.cpu(ProfileTimer::SCENE_RENDER);
	m_stats.cpu_ssr = results.cpu(ProfileTimer::SSR);
	m_stats.cpu_bloom = results.cpu(ProfileTimer::BLOOM);
	m_stats.cpu_post_process = results.cpu(ProfileTimer::POST_PROCESS);
	m_stats.cpu_hiz = results.cpu(ProfileTimer::HIZ);
	m_stats.cpu_shadow_mask = results.cpu(ProfileTimer::SHADOW_MASK);
	m_stats.cpu_outline = results.cpu(ProfileTimer::OUTLINE);
	m_stats.cpu_physics = results.cpu(ProfileTimer::PHYSICS);
	m_stats.cpu_ui = results.cpu(ProfileTimer::UI);
	m_stats.cpu_skinning = results.cpu(ProfileTimer::SKINNING);
	m_stats.cpu_cluster_lights = results.cpu(ProfileTimer::CLUSTER_LIGHTS);
	m_stats.cpu_particles = results.cpu(ProfileTimer::PARTICLES);
}

void RenderPipeline::finalizeFrameTimings() {
	auto& profiler = m_ve_renderer.getProfiler();
	profiler.endCpuTimer(ProfileTimer::FRAME_TOTAL);

	const auto& results = profiler.getResults();
	m_stats.fence_wait = results.fence_wait_ms;
	m_stats.acquire_wait = results.acquire_wait_ms;
	m_stats.cpu_time = results.cpu(ProfileTimer::FRAME_TOTAL) - results.fence_wait_ms - results.acquire_wait_ms;
	m_stats.gpu_time = results.gpu(ProfileTimer::FRAME_TOTAL);
	m_stats.compute_gpu_time = results.gpu(ProfileTimer::COMPUTE_TOTAL);
	m_stats.gpu_overlap = results.gpu_overlap;
}

void RenderPipeline::writeUniformBuffer(uint32_t current_frame, const CameraView& view, UniformBufferObject& ubo) {
	ubo.view = view.view;
	ubo.proj = view.proj;
	ubo.projection_view = ubo.proj * ubo.view;
	ubo.inverse_projection_view = glm::inverse(ubo.projection_view);
	ubo.prev_projection_view = m_prev_projection_view;
	m_prev_projection_view = ubo.projection_view;
	ubo.camera_position = glm::vec4{view.position, 1.0f};
	m_uniform_buffers[current_frame]->writeToBuffer(&ubo);
}

} // namespace ve
