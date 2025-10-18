#pragma once
#include "ve_device.hpp"
#include "ve_pipeline.hpp"
#include "ve_config.hpp"
#include "ve_frame_info.hpp"

#include <memory>
#include <vector>


namespace ve {

	class SkyboxRenderSystem {
	public:
		SkyboxRenderSystem(VeDevice& device, const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout, vk::Format color_format);
		~SkyboxRenderSystem();

		//destroy copy and move constructors and assignment operators
		SkyboxRenderSystem(const SkyboxRenderSystem&) = delete;
		SkyboxRenderSystem& operator=(const SkyboxRenderSystem&) = delete;

		// Slowly rotate the skybox over time and render it
		void render(VeFrameInfo& frame_info);

	private:
		void loadCubeModel();
		void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout);
		void createPipeline(vk::Format color_format);

		VeDevice& m_ve_device;

		vk::raii::PipelineLayout m_pipeline_layout{nullptr};
		std::unique_ptr<VePipeline> m_ve_pipeline;
		VeGameObject m_cube_object = VeGameObject::createGameObject();
	};
}

