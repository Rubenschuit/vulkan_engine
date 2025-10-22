/* VeComputePipeline wraps a compute pipeline creation using Vulkan-Hpp RAII.
It mirrors VePipeline but for compute stage only. */
#pragma once
#include "ve_export.hpp"
#include <vulkan/vulkan_raii.hpp>
#include <vector>

namespace ve { class VeDevice; }




namespace ve {

class VENGINE_API VeComputePipeline {
public:
	VeComputePipeline(VeDevice& device, const std::filesystem::path& comp_spv_path, const vk::raii::PipelineLayout& pipeline_layout);
	~VeComputePipeline() = default;

	VeComputePipeline(const VeComputePipeline&) = delete;
	VeComputePipeline& operator=(const VeComputePipeline&) = delete;

	vk::Pipeline getPipeline() const { return *m_pipeline; }

private:

	void createComputePipeline(const std::filesystem::path& comp_spv_path, const vk::raii::PipelineLayout& pipeline_layout);

	VeDevice& m_ve_device;
	vk::raii::ShaderModule m_shader_module{nullptr};
	vk::raii::Pipeline m_pipeline{nullptr};
};

} // namespace ve
