#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "game/ve_frame_info.hpp"

#include <memory>
#include <filesystem>

namespace ve {
	class VeDevice;
	class VePipeline;
}

namespace ve {

class VENGINE_API PbrRenderSystem {
public:
	PbrRenderSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& material_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path);
	~PbrRenderSystem();

	PbrRenderSystem(const PbrRenderSystem&) = delete;
	PbrRenderSystem& operator=(const PbrRenderSystem&) = delete;

	void renderObjects(VeFrameInfo& frame_info) const;
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
		m_ve_pipeline.reset();
		createPipeline(color_format, sample_count);
	}
	void setTopology(vk::PrimitiveTopology topology) {
		m_topology = topology;
	}

private:
	void createPipelineLayout(
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& material_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout);
	void createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::PrimitiveTopology m_topology = vk::PrimitiveTopology::eTriangleList;
	vk::Format m_color_format = vk::Format::eUndefined;
	vk::SampleCountFlagBits m_sample_count = vk::SampleCountFlagBits::e1;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
};

} // namespace ve

