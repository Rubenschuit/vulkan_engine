#pragma once
#include "ve_device.hpp"
#include "ve_pipeline.hpp"
#include "ve_config.hpp"
#include "ve_frame_info.hpp"

#include <memory>
#include <vector>


namespace ve {

	class SimpleRenderSystem {
	public:
		SimpleRenderSystem(VeDevice& device, const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout, vk::Format color_format);
		~SimpleRenderSystem();

		//destroy copy and move constructors and assignment operators
		SimpleRenderSystem(const SimpleRenderSystem&) = delete;
		SimpleRenderSystem& operator=(const SimpleRenderSystem&) = delete;

		void renderObjects(VeFrameInfo& frame_info) const;

	private:
		void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout);
		void createPipeline(vk::Format color_format);

		VeDevice& m_ve_device;

		vk::raii::PipelineLayout m_pipeline_layout{nullptr};
		std::unique_ptr<VePipeline> m_ve_pipeline;
	};
}

