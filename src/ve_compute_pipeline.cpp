#include "pch.hpp"
#include "ve_compute_pipeline.hpp"
#include "ve_device.hpp"

#include <fstream>
#include <stdexcept>

namespace ve {

VeComputePipeline::VeComputePipeline(VeDevice& device, const char* comp_spv_path, const vk::raii::PipelineLayout& pipeline_layout)
	: m_ve_device(device) {
	createComputePipeline(comp_spv_path, pipeline_layout);
}

std::vector<char> VeComputePipeline::readFile(const char* file_path) {
	std::ifstream file(file_path, std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		throw std::runtime_error(std::string("failed to open file: ") + file_path);
	}
	size_t file_size = static_cast<size_t>(file.tellg());
	std::vector<char> buffer(file_size);
	file.seekg(0);
	file.read(buffer.data(), static_cast<std::streamsize>(file_size));
	file.close();
	return buffer;
}

void VeComputePipeline::createComputePipeline(const char* comp_spv_path, const vk::raii::PipelineLayout& pipeline_layout) {
	auto code = readFile(comp_spv_path);

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
