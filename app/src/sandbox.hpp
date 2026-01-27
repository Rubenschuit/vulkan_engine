#pragma once
#include "VEngine/VEngine.hpp"
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

	// model paths
	const std::filesystem::path project_root;
	std::filesystem::path m_cube_model_path = project_root / "models" / "cube.gltf";
	std::filesystem::path m_viking_room_model_path = project_root / "models" / "viking_room.gltf";
	std::filesystem::path m_quad_model_path = project_root / "models" / "quad.gltf";
	std::filesystem::path m_flat_vase_model_path = project_root / "models" / "flat_vase.gltf";
	std::filesystem::path m_smooth_vase_model_path = project_root / "models" / "smooth_vase.gltf";
	std::filesystem::path m_sphere_model_path = project_root / "models" / "sphere" / "scene.gltf";
	// texture paths
	std::filesystem::path m_glow_texture_path = project_root / "textures" / "light.png";
	std::filesystem::path m_fire_texture_path = project_root / "textures" / "fire_ball.ktx";
	std::filesystem::path m_smoke_texture_path = project_root / "textures" / "smoke_atlas.png";
	std::filesystem::path m_skybox_path = project_root / "textures" / "skybox" / "starfield_haze.ktx";

	// Textures
	VeTexture m_skybox = VeTexture(m_ve_device, m_skybox_path);

	// Scenes
	std::unique_ptr<SimpleScene> m_simple_scene;
	std::unique_ptr<SponzaScene> m_sponza_scene;
	VeScene* m_active_scene = nullptr;

	// UI context captured during renderUI(), consumed in updateParticles() for example.
	UIContext ui_actions;
	std::chrono::steady_clock::time_point m_cpu_start;

	// Render systems
	std::unique_ptr<SkyboxRenderSystem> m_skybox_render_system;
	std::unique_ptr<SimpleRenderSystem> m_simple_render_system;
	std::unique_ptr<PbrRenderSystem> m_pbr_render_system;
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