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

		VeWindow m_ve_window{WIDTH, HEIGHT, "Vulkan Engine!"};
		VeDevice m_ve_device{m_ve_window};
		VeRenderer m_ve_renderer{m_ve_device, m_ve_window};

		std::vector<std::unique_ptr<VeBuffer>> m_uniform_buffers;

		// Descriptor pool, layouts, sets
		std::unique_ptr<VeDescriptorPool> m_global_pool{};
		std::unique_ptr<VeDescriptorSetLayout> m_global_set_layout{};
		std::unique_ptr<VeDescriptorSetLayout> m_material_set_layout{};
		std::vector<vk::raii::DescriptorSet> m_global_descriptor_sets{};
		vk::raii::DescriptorSet m_material_descriptor_set{nullptr};

		// Objects and texture
		const char* m_texture_path = "../textures/viking_room.png";
		VeTexture m_texture{m_ve_device, m_texture_path};
		// model paths hardcoded in .cpp for now
		std::unordered_map<uint32_t, VeGameObject> m_game_objects;

		// Input handling
		InputController m_input_controller{m_ve_window};

		// Camera settings
        VeCamera m_camera{{2.0f, 2.0f, 2.0f}, {0.0f, 0.0f, 1.0f}};
		const float m_fov = glm::radians(55.0f);
		const float m_near_plane = 0.1f;
		const float m_far_plane = 100.0f;
		float m_last_aspect{0.0f};

		// FPS/frametime tracking
		using clock = std::chrono::steady_clock;
		clock::time_point m_last_frame_time{clock::now()};
		clock::time_point m_fps_window_start{clock::now()};
		uint32_t m_fps_frame_count{0};
		double m_sum_frame_ms{0.0};
		double m_last_frame_ms{0.0}; //TODO remove?
		float m_frame_time{0.0f};
	};
}

