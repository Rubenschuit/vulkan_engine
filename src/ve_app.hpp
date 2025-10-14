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
#include "ve_descriptors.hpp"

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
		void loadGameObjects();
		void createUniformBuffers();
		void createDescriptors();

		void updateCamera();
		void updateUniformBuffer(uint32_t current_frame, UniformBufferObject& ubo);
		void updateWindowTitle();

		VeWindow ve_window{WIDTH, HEIGHT, "Vulkan Engine!"};
		VeDevice ve_device{ve_window};
		VeRenderer ve_renderer{ve_device, ve_window};

		std::vector<std::unique_ptr<VeBuffer>> uniform_buffers;

		// Descriptor pool, layouts, sets
		std::unique_ptr<VeDescriptorPool> global_pool{};
		std::unique_ptr<VeDescriptorSetLayout> global_set_layout{};
		std::unique_ptr<VeDescriptorSetLayout> material_set_layout{};
		std::vector<vk::raii::DescriptorSet> global_descriptor_sets{};
		vk::raii::DescriptorSet material_descriptor_set{nullptr};

		// Objects and texture
		VeTexture texture{ve_device, "../textures/viking_room.png"};
		// model paths hardcoded in .cpp for now
		std::unordered_map<uint32_t, VeGameObject> game_objects;

		// Input handling
		InputController input_controller{ve_window};

		// Camera settings
        VeCamera camera{{2.0f, 2.0f, 2.0f}, {0.0f, 0.0f, 1.0f}};
		const float fov = glm::radians(55.0f);
		const float near_plane = 0.1f;
		const float far_plane = 100.0f;
		float last_aspect{0.0f};

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

