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
		void cleanup();
		void loadModels();
		void createPipeline();
		void createPipelineLayout();
		void createCommandBuffers();
		void recordCommandBuffer(uint32_t image_index);
		void transitionImageLayout(
			uint32_t image_index,
			vk::ImageLayout old_layout,
			vk::ImageLayout new_layout,
			vk::AccessFlags2 src_access_mask,
			vk::AccessFlags2 dst_access_mask,
			vk::PipelineStageFlags2 src_stage,
			vk::PipelineStageFlags2 dst_stage);
		void drawFrame();
		void recreateSwapChain();

		VeWindow ve_window{WIDTH, HEIGHT, "Vulkan Engine!"};
		VeDevice ve_device{ve_window};
		std::unique_ptr<VeSwapChain> ve_swap_chain = std::make_unique<VeSwapChain>(ve_device, ve_window.getExtent());
		vk::raii::PipelineLayout pipeline_layout{nullptr};
		std::unique_ptr<VePipeline> ve_pipeline;
		std::vector<vk::raii::CommandBuffer> command_buffers;
		std::unique_ptr<VeModel> ve_model;
	};
}

