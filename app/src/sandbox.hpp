#pragma once
#include "VEngine/VEngine.hpp"
#include "asset_paths.hpp"
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
	void createUniformBuffers();
	void createDescriptors();

	void initSystems();
	void initUI();
	void loadGameObjects();

	void updateParticles(InputActions& actions);
	void renderAppWindows();
	void renderControlsWindow();
	void recreatePipelines();

	// UI context captured during renderUI(), consumed in updateParticles() for example.
	struct SandboxUIContext : public UIContext {
		enum class SceneType { NONE = 0, SIMPLE = 1, SPONZA = 2, SPONZA_LOW = 3 };
		SceneType current_scene = SceneType::SPONZA;
		RenderMode render_mode = RenderMode::BRDF_MICROFACET;

		// ambient light (color RGB + intensity)
		glm::vec3 ambient_light_color = glm::vec3(1.0f, 1.0f, 1.0f);
		float ambient_light_intensity = 0.006f;

		// sponza settings
		float sun_intensity = 2000.0f;

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

	AssetPaths m_paths;

	// Resource manager for models (and textures). Must outlive any ResourceHandles.
	std::unique_ptr<VeResourceManager> m_resource_manager;

	// Shared particle textures (used by particles, point lights, and SimpleScene; always loaded)
	ResourceHandle<VeTexture> m_particle_texture_handle;
	ResourceHandle<VeTexture> m_fire_texture_handle;
	ResourceHandle<VeTexture> m_smoke_texture_handle;
	vk::raii::DescriptorSet m_particle_texture_descriptor_set{nullptr};

	// Scenes - dynamically loaded/unloaded based on active selection
	std::unique_ptr<VeScene> m_active_scene;
	SandboxUIContext::SceneType m_loaded_scene_type = SandboxUIContext::SceneType::NONE;
	SandboxUIContext::SceneType m_pending_scene_load = SandboxUIContext::SceneType::NONE;

	void loadScene(SandboxUIContext::SceneType scene_type);
	void unloadScene();

	SandboxUIContext ui_actions;
	std::chrono::steady_clock::time_point m_cpu_start;

	// Render systems
	std::unique_ptr<CullingSystem> m_culling_system;
	std::unique_ptr<SkyboxRenderSystem> m_skybox_render_system;
	std::unique_ptr<SimpleRenderSystem> m_simple_render_system;
	std::unique_ptr<PbrRenderSystem> m_pbr_render_system;
	std::unique_ptr<AabbDebugRenderSystem> m_aabb_debug_render_system;
	std::unique_ptr<AxesRenderSystem> m_axes_render_system;
	std::unique_ptr<PointLightSystem> m_point_light_system;
	std::unique_ptr<ParticleSystem> m_particle_system;
	std::unique_ptr<FireworksSystem> m_fireworks_system;
	std::unique_ptr<ShadowRenderSystem> m_shadow_render_system;
	std::unique_ptr<PostProcessSystem> m_post_process_system;
	std::unique_ptr<BloomSystem> m_bloom_system;
};

}

// Called by the entry point to create the application instance
ve::VeApplication* createApp(std::filesystem::path project_root);