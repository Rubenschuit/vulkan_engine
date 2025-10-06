#pragma once
#include "ve_window.hpp"
#include "ve_device.hpp"
#include "ve_renderer.hpp"
#include "ve_config.hpp"
#include "ve_game_object.hpp"
#include "ve_texture.hpp"
#include "ve_frame_info.hpp"
#include "input_controller.hpp"
#include "ve_camera.hpp"

#include <memory>
#include <vector>
#include <glm/glm.hpp>

#include <chrono>


namespace ve {

	class VeApp {
	public:
		VeApp();
		~VeApp();

		//destroy copy and move constructors and assignment operators
		VeApp(const VeApp&) = delete;
		VeApp& operator=(const VeApp&) = delete;

		static constexpr int WIDTH = 800;
		static constexpr int HEIGHT = 600;

		void run();

	private:
		void mainLoop();
		void loadGameObjects();
		void createDescriptorSetLayout();
		void createUniformBuffers();
		void createDescriptorPool();
		void createDescriptorSets();

		void updateUniformBuffer(uint32_t current_frame);
		void updateFpsWindowTitle();

		VeWindow ve_window{WIDTH, HEIGHT, "Vulkan Engine!"};
		VeDevice ve_device{ve_window};
		VeRenderer ve_renderer{ve_device, ve_window};

		const char* MODEL_PATH = "../models/viking_room.obj";
		const char* TEXTURE_PATH = "../textures/viking_room.png";
		//const char* TEXTURE_PATH = "../textures/mots.png";

		VeTexture texture{ve_device, TEXTURE_PATH};
		std::unordered_map<uint32_t, VeGameObject> game_objects;

		InputController input_controller{ve_window};
        VeCamera camera{{2.0f, 2.0f, 2.0f}, {0.0f, 0.0f, 1.0f}}; // Y-up by default
		const float fov = glm::radians(55.0f);
		const float near_plane = 0.1f;
		const float far_plane = 100.0f;
		float last_aspect{0.0f};

		vk::raii::DescriptorSetLayout descriptor_set_layout{nullptr};
		std::vector<std::unique_ptr<VeBuffer>> uniform_buffers;
		vk::raii::DescriptorPool descriptor_pool = nullptr;
		std::vector<vk::raii::DescriptorSet> descriptor_sets;

		// FPS/frametime tracking
		using clock = std::chrono::steady_clock;
		clock::time_point last_frame_time{clock::now()};
		clock::time_point fps_window_start{clock::now()};
		uint32_t fps_frame_count{0};
		double sum_frame_ms{0.0};
		double last_frame_ms{0.0}; //TODO remove?
		float frame_time{0.0f};
	};
}

