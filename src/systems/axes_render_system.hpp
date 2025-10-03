/* Class for rendering simple 3D coordinate axes */
#pragma once
#include "ve_device.hpp"
#include "ve_pipeline.hpp"
#include "ve_config.hpp"
#include "ve_frame_info.hpp"
#include "ve_model.hpp"

#include <memory>

namespace ve {

	class AxesRenderSystem {
	public:
		AxesRenderSystem(VeDevice& device, vk::raii::DescriptorSetLayout& descriptor_set_layout, vk::Format color_format);
		~AxesRenderSystem();

		AxesRenderSystem(const AxesRenderSystem&) = delete;
		AxesRenderSystem& operator=(const AxesRenderSystem&) = delete;

		void renderAxes(VeFrameInfo& frame_info) const;

	private:
		void createPipelineLayout(vk::raii::DescriptorSetLayout& descriptor_set_layout);
		void createPipeline(vk::Format color_format);
		void createAxesModel();

		VeDevice& ve_device;
		vk::raii::PipelineLayout pipeline_layout{nullptr};
		std::unique_ptr<VePipeline> ve_pipeline;
		std::unique_ptr<VeModel> axes_model;
	};
}
