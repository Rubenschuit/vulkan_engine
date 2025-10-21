/* Class for rendering simple 3D coordinate axes */
#pragma once
#include "core/ve_device.hpp"
#include "core/ve_pipeline.hpp"
#include "ve_config.hpp"
#include "game/ve_frame_info.hpp"
#include "game/ve_model.hpp"

#include <memory>
#include <string>

namespace ve {

class AxesRenderSystem {
public:
	AxesRenderSystem( VeDevice& device,
					  const vk::raii::DescriptorSetLayout& descriptor_set_layout,
					  vk::Format color_format,
					  std::filesystem::path shader_path);
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
	std::string m_shader_path;
};
}
