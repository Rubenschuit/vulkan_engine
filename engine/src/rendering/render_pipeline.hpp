#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "events/event_bus.hpp"
#include "rendering/render_services.hpp"
#include "rendering/render_settings.hpp"
#include "rendering/frame_stats.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace ve {

class VeDevice;
class VeRenderer;
class VeResourceManager;
class VeBuffer;
class SceneResourceManager;
class RenderResources;
class SettingsWatcher;
class VeScene;
class Registry;
class CullingBackend;
struct EngineConfig;
struct SceneContext;
struct UniformBufferObject;
struct CameraView;
struct EditorState;
struct VeFrameInfo;

class CullingSystem;
class ShadowRenderSystem;
class DepthPrePassSystem;
class ShadowMaskSystem;
class GtaoSystem;
class ClusterLightSystem;
class PbrRenderSystem;
class AabbDebugRenderSystem;
class AxesRenderSystem;
class LightSystem;
class ParticleBackend;
class ParticleEmitterSystem;
class SkyboxRenderSystem;
class IblSystem;
class BloomSystem;
class PostProcessSystem;
class OutlineSystem;
class GpuCullingSystem;
class MeshletCullingSystem;
class HizSystem;
class DeformPrePass;
class SkinnedPointsRenderSystem;
class CpuCullingBackend;
class GpuCullingBackend;
class MeshletCullingBackend;

class VENGINE_API RenderPipeline {
public:
	RenderPipeline(VeDevice& device,
	               VeRenderer& renderer,
	               VeResourceManager& resource_manager,
	               RenderResources& resources,
	               EventBus& event_bus,
	               const EngineConfig& config);
	~RenderPipeline();

	RenderPipeline(const RenderPipeline&) = delete;
	RenderPipeline& operator=(const RenderPipeline&) = delete;

	static constexpr uint32_t INITIAL_INSTANCE_CAPACITY = 16384 * 2;

	// Apply per-frame UI settings and emit setting-change events.
	void prepareFrame();
	void renderFrame(VeScene& scene,
	                 const CameraView& camera_view,
	                 const EditorState& editor_state,
	                 float frame_time,
	                 float total_time);
	void finalizeFrameTimings();

	const RenderServices& services() const { return m_services; }

	RenderSettings& settings() { return m_settings; }
	const RenderSettings& settings() const { return m_settings; }
	FrameStats& stats() { return m_stats; }
	const FrameStats& stats() const { return m_stats; }

private:
	void initRenderSystems();
	void emitSwapChainRecreatedEvents();
	void emitResolutionChangedEvent();
	void pushPerFrameSettings();
	void selectBackend();
	void ensureHizInfrastructure();
	void ensureGpuCullingInfrastructure();
	void ensureMeshletCullingInfrastructure();
	void tearDownHizInfrastructure();
	void tearDownGpuCullingInfrastructure();
	void tearDownMeshletCullingInfrastructure();
	VeFrameInfo buildFrameInfo(VeScene& scene,
	                           const CameraView& camera_view,
	                           const EditorState& editor_state,
	                           float frame_time,
	                           float total_time);
	void populateUBO(VeFrameInfo& fi);
	void dispatchCompute(VeFrameInfo& fi);
	void renderFrameBody(VeFrameInfo& fi, const EditorState& editor_state);
	void collectStats(const VeFrameInfo& fi, Registry& registry);
	void writeUniformBuffer(uint32_t current_frame, const CameraView& view, UniformBufferObject& ubo);
	void createPerFrameResources(const VeBuffer& material_ssbo);

	VeDevice& m_ve_device;
	VeRenderer& m_ve_renderer;
	VeResourceManager& m_resource_manager;
	RenderResources& m_resources;
	EventBus& m_event_bus;
	RenderSettings m_settings;
	FrameStats m_stats;
	const EngineConfig& m_config;

	std::unique_ptr<SceneResourceManager> m_scene_resources;

	std::vector<std::unique_ptr<VeBuffer>> m_uniform_buffers;
	std::vector<std::unique_ptr<VeBuffer>> m_instance_buffers;
	std::vector<vk::raii::DescriptorSet> m_global_descriptor_sets;

	std::unique_ptr<CullingSystem> m_culling_system;
	std::unique_ptr<ShadowRenderSystem> m_shadow_render_system;
	std::unique_ptr<DepthPrePassSystem> m_depth_prepass_system;
	std::unique_ptr<ShadowMaskSystem> m_shadow_mask_system;
	std::unique_ptr<GtaoSystem> m_gtao_system;
	std::unique_ptr<ClusterLightSystem> m_cluster_light_system;
	std::unique_ptr<PbrRenderSystem> m_pbr_render_system;
	std::unique_ptr<AabbDebugRenderSystem> m_aabb_debug_render_system;
	std::unique_ptr<AxesRenderSystem> m_axes_render_system;
	std::unique_ptr<LightSystem> m_light_system;
	std::unique_ptr<ParticleBackend> m_particle_backend;
	std::unique_ptr<ParticleEmitterSystem> m_particle_emitter_system;
	std::unique_ptr<SkyboxRenderSystem> m_skybox_render_system;
	std::unique_ptr<IblSystem> m_ibl_system;
	std::unique_ptr<BloomSystem> m_bloom_system;
	std::unique_ptr<PostProcessSystem> m_post_process_system;
	std::unique_ptr<OutlineSystem> m_outline_system;
	std::unique_ptr<GpuCullingSystem> m_gpu_culling_system;
	std::unique_ptr<MeshletCullingSystem> m_meshlet_culling_system;
	std::unique_ptr<HizSystem> m_hiz_system;
	std::unique_ptr<DeformPrePass> m_deform_pre_pass;
	std::unique_ptr<SkinnedPointsRenderSystem> m_skinned_points_render_system;

	std::unique_ptr<CpuCullingBackend> m_cpu_backend;
	std::unique_ptr<GpuCullingBackend> m_gpu_backend;
	std::unique_ptr<MeshletCullingBackend> m_meshlet_backend;
	CullingBackend* m_active_backend = nullptr;
	uint32_t m_gpu_inactive_frames = 0;
	uint32_t m_meshlet_inactive_frames = 0;

	std::unique_ptr<SettingsWatcher> m_settings_watcher;

	RenderServices m_services{};

	glm::mat4 m_prev_projection_view{1.0f};


	EventSubscriptionId m_scene_loaded_sub = 0;
	EventSubscriptionId m_swap_chain_recreated_sub = 0;
	EventSubscriptionId m_viewport_resized_sub = 0;
};

} // namespace ve
