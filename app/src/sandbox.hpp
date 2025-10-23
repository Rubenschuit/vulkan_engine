#pragma once
#include "VEngine/VEngine.hpp"
#include <filesystem>

//TODO move over more to base class

namespace ve {

class Sandbox : public VeApplication {
public:
	Sandbox(const std::filesystem::path& working_dir);
	~Sandbox();

	//destroy copy and move constructors and assignment operators
	Sandbox(const Sandbox&) = delete;
	Sandbox& operator=(const Sandbox&) = delete;

	void run() override;

private:
	void loadGameObjects();
	void createUniformBuffers();
	void createDescriptors();
	void initSystems();
	void initUI();

	void updateParticles(VeFrameInfo& frame_info, InputActions& actions);
	void renderScene(VeFrameInfo& frame_info);

	const std::filesystem::path working_directory;

	// Object paths
	std::filesystem::path m_cube_model_path;
	std::filesystem::path m_viking_room_model_path;
	std::filesystem::path m_quad_model_path;
	std::filesystem::path m_flat_vase_model_path;
	std::filesystem::path m_smooth_vase_model_path;

	// Texture paths
	std::filesystem::path m_texture_path;
	std::vector<std::filesystem::path> m_skybox_paths;
	/*
	const char* m_skybox_path[6] = {working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png" };
	*/


	VeTexture m_skybox;
	VeTexture m_texture;

	// Game objects
	std::unordered_map<uint32_t, VeGameObject> m_game_objects;

	// UI context captured during renderUI(), consumed in updateParticles() for example.
	UIContext ui_context;

	// Render systems 
	std::unique_ptr<SkyboxRenderSystem> m_skybox_render_system;
	std::unique_ptr<SimpleRenderSystem> m_simple_render_system;
	std::unique_ptr<AxesRenderSystem> m_axes_render_system;
	std::unique_ptr<PointLightSystem> m_point_light_system;
	std::unique_ptr<ParticleSystem> m_particle_system;
};

} // namespace ve


// Called by the entry point to create the application instance
ve::VeApplication* createApp(std::filesystem::path working_directory) {
	return new ve::Sandbox(working_directory);
}