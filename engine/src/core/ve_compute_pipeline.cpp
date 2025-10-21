#include "pch.hpp"
#include "ve_compute_pipeline.hpp"
#include "ve_device.hpp"
#include "ve_file_system.hpp"


namespace ve {

VeComputePipeline::VeComputePipeline(VeDevice& device, const std::filesystem::path& comp_spv_path, const vk::raii::PipelineLayout& pipeline_layout)
	: m_ve_device(device) {
	createComputePipeline(comp_spv_path, pipeline_layout);
}

void VeComputePipeline::createComputePipeline(const std::filesystem::path& comp_spv_path, const vk::raii::PipelineLayout& pipeline_layout) {
	auto code = VeFileSystem::readFile(comp_spv_path);

	vk::ShaderModuleCreateInfo create_info{
		.codeSize = code.size(),
		.pCode = reinterpret_cast<const uint32_t*>(code.data())
	};
	m_shader_module = vk::raii::ShaderModule(m_ve_device.getDevice(), create_info);

	vk::PipelineShaderStageCreateInfo stage_info{
		.stage = vk::ShaderStageFlagBits::eCompute,
		.module = *m_shader_module,
		.pName = "compMain"
	};

	vk::ComputePipelineCreateInfo pipeline_info{
		.stage = stage_info,
		.layout = *pipeline_layout
	};

	m_pipeline = vk::raii::Pipeline{m_ve_device.getDevice(), nullptr, pipeline_info};
}

} // namespace ve
