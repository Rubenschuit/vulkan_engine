#pragma once

#include "application/ve_engine_config.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_device.hpp"
#include "rendering/ve_renderer.hpp"
#include "ui/imgui_layer.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "input/input_controller.hpp"
#include "scene/ve_camera.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"
#include "resources/ve_material_properties.hpp"
#include <memory>
#include <vector>
#include <chrono>
#include <functional>
#include <filesystem>

namespace ve {

// Scene registry: apps register named scene factories, editor provides the UI to switch
struct VENGINE_API SceneEntry {
	std::string name;
	std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory;
};

// Written by editor, consumed by VeApplication main loop (one-frame deferred)
struct VENGINE_API SceneLoadRequest {
	enum class Type { NONE, LOAD_REGISTERED, LOAD_GLTF_PATH, NEW_EMPTY, ADD_MODEL };
	Type type = Type::NONE;
	int scene_index = -1;
	std::filesystem::path gltf_path;
};

// Forward declarations
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
class ParticleSystem;
class FireworksSystem;
class SkyboxRenderSystem;
class BloomSystem;
class PostProcessSystem;
class OutlineSystem;
class SceneResourceManager;
class GpuCullingSystem;
class MeshletCullingSystem;
class HizSystem;
class Editor;
class VeScene;
class CullingBackend;
class CpuCullingBackend;
class GpuCullingBackend;
class MeshletCullingBackend;

class VENGINE_API VeApplication {
public:

	static constexpr int WIDTH = 1920;
	static constexpr int HEIGHT = 1080;
	const char* APP_NAME = "Vulkan Engine!";

	explicit VeApplication(const EngineConfig& config);
	virtual ~VeApplication();

	VeApplication(const VeApplication&) = delete;
	VeApplication& operator=(const VeApplication&) = delete;

	// Called by main in entry point to start the application loop
	void run();

protected:
	// --- App overrides ---
	virtual void update() {}
	virtual void renderUI() {}

	// --- Scene management ---
	void registerScene(const std::string& name,
					   std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory);
	void loadDefaultScene(int index);
	void setActiveScene(std::unique_ptr<VeScene> scene);
	VeScene* getActiveScene() { return m_active_scene.get(); }
	Registry* getActiveRegistry();
	void unloadScene();

	// --- Scene construction context ---
	SceneContext getSceneContext() {
		return {m_ve_device, *m_resource_manager, *m_global_pool,
				*m_material_set_layout, &m_default_material_descriptor_set};
	}

	// --- Access systems (for app-specific config) ---
	ParticleSystem& getParticleSystem() { return *m_particle_system; }
	FireworksSystem& getFireworksSystem() { return *m_fireworks_system; }
	SkyboxRenderSystem& getSkyboxSystem() { return *m_skybox_render_system; }
	PbrRenderSystem& getPbrSystem() { return *m_pbr_render_system; }
	Editor& getEditor() { return *m_editor; }

	// --- App name ---
	const std::string& getAppSettingsWindowName() const;

	// --- Settings struct (app writes, engine reads) ---
	UIContext& ui() { return m_ui; }

	// --- Input (last frame's actions, for app-specific key handling) ---
	const InputActions& getInputActions() const { return m_last_input_actions; }

	// Engine-managed state accessible to app
	VeWindow m_ve_window;
	VeDevice m_ve_device;
	VeRenderer m_ve_renderer;
	VeCamera m_camera;
	InputController m_input_controller;

private:
	EngineConfig m_config;

	// --- Initialization (called from constructor) ---
	void createBuffers();
	void createDescriptors();
	void initSystems();
	void initEditor();

	// --- Main loop ---
	VeFrameInfo buildFrameInfo();
	void selectBackend();
	void applySettingChanges();
	void populateUBO(VeFrameInfo& fi);
	void dispatchCompute(VeFrameInfo& fi);
	void renderFrame(VeFrameInfo& fi);
	void collectStats(const VeFrameInfo& fi);
	void onSwapChainRecreated();
	void recreatePipelines();
	void recreateResolutionDependentSystems();

	// --- Camera ---
	void updateCamera(float fov_radians);
	void updateUniformBuffer(uint32_t current_frame, UniformBufferObject& ubo);
	void updateFrameTime();

	UIContext m_ui;

	std::unique_ptr<VeResourceManager> m_resource_manager;

	// --- Buffers ---
	std::vector<std::unique_ptr<VeBuffer>> m_uniform_buffers{};
	std::vector<std::unique_ptr<VeBuffer>> m_instance_buffers{};
	static constexpr uint32_t INITIAL_INSTANCE_CAPACITY = 16384;

	// --- Descriptors ---
	std::shared_ptr<VeDescriptorPool> m_global_pool{};
	std::unique_ptr<VeDescriptorSetLayout> m_global_set_layout{};
	std::unique_ptr<VeDescriptorSetLayout> m_material_set_layout{};
	std::vector<vk::raii::DescriptorSet> m_global_descriptor_sets{};

	// Default material (engine-generated solid-color PBR textures)
	std::unique_ptr<VeBuffer> m_default_material_ubo;
	ResourceHandle<VeTexture> m_default_albedo_handle;
	ResourceHandle<VeTexture> m_default_normal_handle;
	ResourceHandle<VeTexture> m_default_mr_handle;
	ResourceHandle<VeTexture> m_default_occlusion_handle;
	ResourceHandle<VeTexture> m_default_emissive_handle;
	vk::raii::DescriptorSet m_default_material_descriptor_set{nullptr};

	// Particle textures
	ResourceHandle<VeTexture> m_particle_texture_handle;
	ResourceHandle<VeTexture> m_fire_texture_handle;
	ResourceHandle<VeTexture> m_smoke_texture_handle;
	vk::raii::DescriptorSet m_particle_descriptor_set{nullptr};

	// --- Scene ---
	std::unique_ptr<VeScene> m_active_scene;
	std::vector<SceneEntry> m_scene_entries;
	int m_current_scene_index = -1;
	int m_loaded_scene_index = -1;
	SceneLoadRequest m_pending_load;
	void processSceneLoadRequest();

	// --- UI ---
	std::unique_ptr<ImGuiLayer> m_imgui_layer;
	std::unique_ptr<Editor> m_editor;

	// --- Render systems ---
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
	std::unique_ptr<ParticleSystem> m_particle_system;
	std::unique_ptr<FireworksSystem> m_fireworks_system;
	std::unique_ptr<SkyboxRenderSystem> m_skybox_render_system;
	std::unique_ptr<BloomSystem> m_bloom_system;
	std::unique_ptr<PostProcessSystem> m_post_process_system;
	std::unique_ptr<OutlineSystem> m_outline_system;
	std::unique_ptr<SceneResourceManager> m_scene_resources;
	std::unique_ptr<GpuCullingSystem> m_gpu_culling_system;
	std::unique_ptr<MeshletCullingSystem> m_meshlet_culling_system;
	std::unique_ptr<HizSystem> m_hiz_system;

	// --- Culling backends ---
	std::unique_ptr<CpuCullingBackend> m_cpu_backend;
	std::unique_ptr<GpuCullingBackend> m_gpu_backend;
	std::unique_ptr<MeshletCullingBackend> m_meshlet_backend;
	CullingBackend* m_active_backend = nullptr;

	// --- Camera state ---
	glm::mat4 m_prev_projection_view{1.0f};
	float m_fov = glm::radians(80.0f);
	float m_near_plane = 0.1f;
	float m_far_plane = 100000.0f;
	float m_last_aspect{0.0f};

	// --- Timing ---
	using clock = std::chrono::steady_clock;
	clock::time_point m_last_frame_time{clock::now()};
	float m_total_time{0.0f};
	float m_frame_time{0.0f};

	InputActions m_last_input_actions;
	bool m_shadow_mask_half_res = false;
	bool m_gtao_half_res = true;
	int m_pcf_samples = 8;
	int m_pcss_filter_samples = 16;
	Topology m_last_topology = Topology::TRIANGLE_LIST;

	void setWindowTitle();
};

} // namespace ve
