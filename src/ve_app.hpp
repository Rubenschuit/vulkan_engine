#pragma once
#include <ve_window.hpp>
#include <ve_device.hpp>
#include <ve_pipeline.hpp>
#include <ve_swap_chain.hpp>
#include <ve_config.hpp>
#include <ve_model.hpp>

#include <memory>
#include <vector>
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>


namespace ve {

	struct UniformBufferObject {
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;
	};

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
		void cleanup();
		void loadModels();
		void createPipeline();
		void createDescriptorSetLayout();
		void createPipelineLayout();
		void createCommandBuffers();
		void recordCommandBuffer(uint32_t image_index);
		void drawFrame();
		void recreateSwapChain();
		void createUniformBuffers();
		void updateUniformBuffer(uint32_t current_image);
		void createDescriptorPool();
		void createDescriptorSets();
		void updateFpsWindowTitle();


		VeWindow ve_window{WIDTH, HEIGHT, "Vulkan Engine!"};
		VeDevice ve_device{ve_window};
		std::unique_ptr<VeSwapChain> ve_swap_chain = std::make_unique<VeSwapChain>(ve_device, ve_window.getExtent());

		vk::raii::DescriptorSetLayout descriptor_set_layout{nullptr};
		vk::raii::PipelineLayout pipeline_layout{nullptr};
		std::unique_ptr<VePipeline> ve_pipeline;
		std::vector<vk::raii::CommandBuffer> command_buffers;
		std::unique_ptr<VeModel> ve_model;

		std::vector<std::unique_ptr<VeBuffer>> uniform_buffers;
		vk::raii::DescriptorPool descriptor_pool = nullptr;
		std::vector<vk::raii::DescriptorSet> descriptor_sets;

		// FPS tracking
		std::chrono::high_resolution_clock::time_point fps_last_time{std::chrono::high_resolution_clock::now()};
		uint32_t fps_frame_count{0};

	};
}

