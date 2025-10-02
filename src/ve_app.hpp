#pragma once
#include "ve_window.hpp"
#include "ve_device.hpp"
#include "ve_renderer.hpp"
#include "ve_config.hpp"
#include "ve_model.hpp"
#include "ve_texture.hpp"
#include "ve_frame_info.hpp"

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
		void loadModels();
		void createDescriptorSetLayout();
		void createUniformBuffers();
		void createDescriptorPool();
		void createDescriptorSets();

		void updateUniformBuffer(uint32_t current_frame);
		void updateFpsWindowTitle();


		VeWindow ve_window{WIDTH, HEIGHT, "Vulkan Engine!"};
		VeDevice ve_device{ve_window};
		VeRenderer ve_renderer{ve_device, ve_window};

		VeTexture texture{ve_device, "../textures/statue.jpeg"};

		vk::raii::DescriptorSetLayout descriptor_set_layout{nullptr};

		std::unique_ptr<VeModel> ve_model;

		std::vector<std::unique_ptr<VeBuffer>> uniform_buffers;
		vk::raii::DescriptorPool descriptor_pool = nullptr;
		std::vector<vk::raii::DescriptorSet> descriptor_sets;

		// FPS/frametime tracking
		using clock = std::chrono::steady_clock;
		clock::time_point last_frame_time{clock::now()};
		clock::time_point fps_window_start{clock::now()};
		uint32_t fps_frame_count{0};
		double sum_frame_ms{0.0};
		double last_frame_ms{0.0};

	};
}

