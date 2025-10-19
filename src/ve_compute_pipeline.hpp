/* VeComputePipeline wraps a compute pipeline creation using Vulkan-Hpp RAII.
It mirrors VePipeline but for compute stage only. */
#pragma once

#include <vulkan/vulkan_raii.hpp>

namespace ve { class VeDevice; }

#include <vector>

namespace ve {

class VeComputePipeline {
public:
	VeComputePipeline(VeDevice& device, const char* comp_spv_path, const vk::raii::PipelineLayout& pipeline_layout);
	~VeComputePipeline() = default;

	VeComputePipeline(const VeComputePipeline&) = delete;
	VeComputePipeline& operator=(const VeComputePipeline&) = delete;

	vk::Pipeline getPipeline() const { return *m_pipeline; }

private:
	static std::vector<char> readFile(const char* file_path);
	void createComputePipeline(const char* comp_spv_path, const vk::raii::PipelineLayout& pipeline_layout);

	VeDevice& m_ve_device;
	vk::raii::ShaderModule m_shader_module{nullptr};
	vk::raii::Pipeline m_pipeline{nullptr};
};

} // namespace ve
