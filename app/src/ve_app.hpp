#pragma once
#include "VEngine/VEngine.hpp"
#include "pch.hpp"

namespace ve {

class VeApp {
public:
	VeApp(const std::filesystem::path& working_dir);
	~VeApp();

	//destroy copy and move constructors and assignment operators
	VeApp(const VeApp&) = delete;
	VeApp& operator=(const VeApp&) = delete;

	static constexpr int WIDTH = 1920;
	static constexpr int HEIGHT = 1080;
	const char* APP_NAME = "Vulkan Engine!";

	void run();

private:
	void loadGameObjects();
	void createUniformBuffers();
	void createDescriptors();
	void initSystems();
	void initUI();

	void updateCamera();
	void updateUniformBuffer(uint32_t current_frame, UniformBufferObject& ubo);
	void updateWindowTitle();
	void updateParticles(VeFrameInfo& frame_info, InputActions& actions);
	void renderScene(VeFrameInfo& frame_info);

	const std::filesystem::path working_directory;

	VeWindow m_ve_window{WIDTH, HEIGHT, APP_NAME};
	VeDevice m_ve_device{m_ve_window};
	VeRenderer m_ve_renderer{m_ve_device, m_ve_window};
	std::unique_ptr<ImGuiLayer> imgui_layer{}; // created in cpp
	std::vector<std::unique_ptr<VeBuffer>> m_uniform_buffers{};

	// Descriptor pool, layouts, sets
	std::shared_ptr<VeDescriptorPool> m_global_pool{};

	std::unique_ptr<VeDescriptorSetLayout> m_global_set_layout{};
	std::unique_ptr<VeDescriptorSetLayout> m_material_set_layout{};

	std::vector<vk::raii::DescriptorSet> m_global_descriptor_sets{};
	vk::raii::DescriptorSet m_material_descriptor_set{nullptr};
	vk::raii::DescriptorSet m_cubemap_descriptor_set{nullptr};

	// Objects
	std::filesystem::path  m_cube_model_path = working_directory / "models" / "cube.obj";
	std::filesystem::path  m_viking_room_model_path = working_directory / "models" / "viking_room.obj";
	std::filesystem::path  m_quad_model_path = working_directory / "models" / "quad.obj";
	std::filesystem::path  m_flat_vase_model_path = working_directory / "models" / "flat_vase.obj";
	std::filesystem::path  m_smooth_vase_model_path = working_directory / "models" / "smooth_vase.obj";

	// Textures
	std::filesystem::path m_texture_path = working_directory / "textures" / "viking_room.png";
	std::vector<std::filesystem::path>  m_skybox_paths = {working_directory / "textures" / "skybox" / "Starfield_And_Haze_left.png",
									working_directory / "textures" / "skybox" / "Starfield_And_Haze_right.png",
									working_directory / "textures" / "skybox" / "Starfield_And_Haze_up.png",
									working_directory / "textures" / "skybox" / "Starfield_And_Haze_down.png",
									working_directory / "textures" / "skybox" / "Starfield_And_Haze_front.png",
									working_directory / "textures" / "skybox" / "Starfield_And_Haze_back.png" };
		/*
	const char* m_skybox_path[6] = {working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png",
									working_directory + "/textures/mots.png" };
									*/
	VeTexture m_skybox{m_ve_device, m_skybox_paths};
	VeTexture m_texture{m_ve_device, m_texture_path};


	std::unordered_map<uint32_t, VeGameObject> m_game_objects;

	// Input handling
	InputController m_input_controller{m_ve_window};

	// Camera settings
	VeCamera m_camera{{20.0f, 20.0f, 20.0f}, {0.0f, 0.0f, 1.0f}};
	float m_fov = glm::radians(80.0f);
	float m_near_plane = 0.1f;
	float m_far_plane = 100000.0f;
	float m_last_aspect{0.0f};

	// FPS/frametime tracking
	using clock = std::chrono::steady_clock;
	clock::time_point m_last_frame_time{clock::now()};
	clock::time_point m_fps_window_start{clock::now()};
	uint32_t m_fps_frame_count{0};
	double m_sum_frame_ms{0.0};
	double m_last_frame_ms{0.0}; //TODO remove?
	float m_frame_time{0.0f};

	// UI context captured during renderUI(), consumed in updateParticles()
	// for example
	UIContext ui_context;

	// Render systems as members
	std::unique_ptr<SkyboxRenderSystem> m_skybox_render_system;
	std::unique_ptr<SimpleRenderSystem> m_simple_render_system;
	std::unique_ptr<AxesRenderSystem> m_axes_render_system;
	std::unique_ptr<PointLightSystem> m_point_light_system;
	std::unique_ptr<ParticleSystem> m_particle_system;
};
}

