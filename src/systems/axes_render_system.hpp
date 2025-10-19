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
	AxesRenderSystem(VeDevice& device, const vk::raii::DescriptorSetLayout& descriptor_set_layout, vk::Format color_format);
	~AxesRenderSystem();

	AxesRenderSystem(const AxesRenderSystem&) = delete;
	AxesRenderSystem& operator=(const AxesRenderSystem&) = delete;

	void render(VeFrameInfo& frame_info) const;

private:
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& descriptor_set_layout);
	void createPipeline(vk::Format color_format);
	void createAxesModel();

	VeDevice& m_ve_device;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
	std::unique_ptr<VeModel> m_axes_model;
};
}
