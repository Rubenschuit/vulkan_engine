#include "pch.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_debug_utils.hpp"
#include "platform/ve_file_system.hpp"


namespace ve {

VeComputePipeline::VeComputePipeline(VeDevice& device,
                                     const std::filesystem::path& comp_spv_path,
                                     const vk::raii::PipelineLayout& pipeline_layout,
                                     const std::unordered_map<uint32_t, uint32_t>& specialization_constants)
	: m_ve_device(device) {
	createComputePipeline(comp_spv_path, pipeline_layout, specialization_constants);
}

void VeComputePipeline::createComputePipeline(const std::filesystem::path& comp_spv_path,
                                              const vk::raii::PipelineLayout& pipeline_layout,
                                              const std::unordered_map<uint32_t, uint32_t>& specialization_constants) {
	auto code = VeFileSystem::readFile(comp_spv_path);

	vk::ShaderModuleCreateInfo create_info{
		.codeSize = code.size(),
		.pCode = reinterpret_cast<const uint32_t*>(code.data())
	};
	m_shader_module = vk::raii::ShaderModule(m_ve_device.getDevice(), create_info);

	std::vector<vk::SpecializationMapEntry> spec_map_entries;
	std::vector<uint32_t> spec_data;
	vk::SpecializationInfo spec_info{};

	if (!specialization_constants.empty()) {
		spec_map_entries.reserve(specialization_constants.size());
		spec_data.reserve(specialization_constants.size());

		uint32_t index = 0;
		for (const auto& [constant_id, value] : specialization_constants) {
			spec_map_entries.push_back({
				.constantID = constant_id,
				.offset = static_cast<uint32_t>(index * sizeof(uint32_t)),
				.size = sizeof(uint32_t)
			});
			spec_data.push_back(value);
			index++;
		}

		spec_info = {
			.mapEntryCount = static_cast<uint32_t>(spec_map_entries.size()),
			.pMapEntries = spec_map_entries.data(),
			.dataSize = spec_data.size() * sizeof(uint32_t),
			.pData = spec_data.data()
		};
	}

	const auto* p_spec_info = specialization_constants.empty() ? nullptr : &spec_info;

	vk::PipelineShaderStageCreateInfo stage_info{
		.stage = vk::ShaderStageFlagBits::eCompute,
		.module = *m_shader_module,
		.pName = "compMain",
		.pSpecializationInfo = p_spec_info
	};

	vk::ComputePipelineCreateInfo pipeline_info{
		.stage = stage_info,
		.layout = *pipeline_layout
	};

	m_pipeline = vk::raii::Pipeline{m_ve_device.getDevice(), m_ve_device.getPipelineCache(), pipeline_info};
	setDebugName(m_ve_device, m_pipeline, comp_spv_path.stem().string().c_str());
}

} // namespace ve
