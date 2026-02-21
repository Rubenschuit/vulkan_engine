#pragma once
#include "VEngine/VEngine.hpp"
#include "rendering/cluster_light_system.hpp"
#include "rendering/depth_prepass_system.hpp"
#include "rendering/gtao_system.hpp"
#include "rendering/shadow_mask_system.hpp"
#include "asset_paths.hpp"
#include "scenes/bistro_scene.hpp"
#include "scenes/gltf_scene.hpp"
#include "scenes/simple_scene.hpp"
#include "scenes/sponza_scene.hpp"
#include <filesystem>
#include <memory>

namespace ve {

class Sandbox : public VeApplication {
public:
	Sandbox(const std::filesystem::path& working_dir);
	~Sandbox();

	Sandbox(const Sandbox&) = delete;
	Sandbox& operator=(const Sandbox&) = delete;

	virtual VeFrameInfo update() override;
	virtual void render(VeFrameInfo& frame_info) override;
	virtual void onSwapChainRecreated() override;

private:
	void createBuffers();
	void createDescriptors();

	void initSystems();
	void initUI();
	void loadGameObjects();

	void updateParticles(InputActions& actions);
	void renderAppWindows();
	void renderControlsWindow();
	void recreatePipelines();

	AssetPaths m_paths;

	// Resource manager for models (and textures). Must outlive any ResourceHandles.
	std::unique_ptr<VeResourceManager> m_resource_manager;

	// Default material UBO for untextured meshes (binding 5 of material layout).
	std::unique_ptr<VeBuffer> m_default_material_ubo;
	// Particle textures (glow, fire, smoke) for particle system and point lights.
	ResourceHandle<VeTexture> m_particle_texture_handle;
	ResourceHandle<VeTexture> m_fire_texture_handle;
	ResourceHandle<VeTexture> m_smoke_texture_handle;
	// Default PBR textures: must outlive descriptor sets that reference them.
	ResourceHandle<VeTexture> m_default_albedo_handle;
	ResourceHandle<VeTexture> m_default_normal_handle;
	ResourceHandle<VeTexture> m_default_mr_handle;
	ResourceHandle<VeTexture> m_default_occlusion_handle;
	ResourceHandle<VeTexture> m_default_emissive_handle;
	// Particle descriptor set: used by particle system and point light system. All scenes.
	vk::raii::DescriptorSet m_particle_descriptor_set{nullptr};
	// Default material descriptor set: untextured fallback
	vk::raii::DescriptorSet m_default_material_descriptor_set{nullptr};

	// UI context captured during renderUI(), consumed in updateParticles() for example.
	struct SandboxUIContext : public UIContext {
		enum class SceneType { NONE = 0, SIMPLE = 1, SPONZA = 2, SPONZA_LOW = 3, BISTRO = 4, GLTF = 5 };
		SceneType current_scene = SceneType::SIMPLE;
		RenderMode render_mode = RenderMode::BRDF_MICROFACET;

		// ambient light (color RGB + intensity)
		glm::vec3 ambient_light_color = glm::vec3(1.0f, 1.0f, 1.0f);
		float ambient_light_intensity = 0.006f;

		// Sun intensity (per scene; initialised from active scene in loadScene)
		float sun_intensity = 0.0f;

		// clustered lighting
		bool cluster_enabled = true;

		// particle system
		ParticleMode current_mode = ParticleMode::COOL;
		float speed = 1.0f;

		// particle count
		uint32_t pending_particle_count = 10000;
		bool apply_particle_count = false;
		bool reset_particle_count = false;

		// particle explosion normal dist
		float particle_velocity_mean = 0.0f;
		float particle_velocity_stddev = 1.0f;
		bool apply_velocity_params = false;

		// lifetime
		float min_life = 1.0f;
		float max_life = 3.0f;
		bool should_respawn = true;

		// emission
		bool emit_burst = false;
		int emit_count = 1000;
	};
	SandboxUIContext ui_actions;

	// Scenes - dynamically loaded/unloaded based on active selection
	std::unique_ptr<VeScene> m_active_scene;
	SandboxUIContext::SceneType m_loaded_scene_type = SandboxUIContext::SceneType::NONE;
	SandboxUIContext::SceneType m_pending_scene_load = SandboxUIContext::SceneType::NONE;

	void loadScene(SandboxUIContext::SceneType scene_type);
	void unloadScene();

	std::filesystem::path m_pending_gltf_path;

	std::chrono::steady_clock::time_point m_cpu_start;
	bool m_shadow_mask_half_res = true; // tracks UI toggle for recreation
	bool m_gtao_half_res = true;        // tracks UI toggle for GTAO recreation
	int m_pcf_samples = 8;
	int m_pcss_filter_samples = 16;

	// Render systems
	std::unique_ptr<CullingSystem> m_culling_system;
	std::unique_ptr<SkyboxRenderSystem> m_skybox_render_system;
	std::unique_ptr<SimpleRenderSystem> m_simple_render_system;
	std::unique_ptr<PbrRenderSystem> m_pbr_render_system;
	std::unique_ptr<AabbDebugRenderSystem> m_aabb_debug_render_system;
	std::unique_ptr<AxesRenderSystem> m_axes_render_system;
	std::unique_ptr<LightSystem> m_light_system;
	std::unique_ptr<ParticleSystem> m_particle_system;
	std::unique_ptr<FireworksSystem> m_fireworks_system;
	std::unique_ptr<ShadowRenderSystem> m_shadow_render_system;
	std::unique_ptr<DepthPrePassSystem> m_depth_prepass_system;
	std::unique_ptr<PostProcessSystem> m_post_process_system;
	std::unique_ptr<BloomSystem> m_bloom_system;
	std::unique_ptr<ShadowMaskSystem> m_shadow_mask_system;
	std::unique_ptr<GtaoSystem> m_gtao_system;
	std::unique_ptr<ClusterLightSystem> m_cluster_light_system;
};

}

// Called by the entry point to create the application instance
ve::VeApplication* createApp(std::filesystem::path project_root);