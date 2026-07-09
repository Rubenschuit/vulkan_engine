#include "pch.hpp"
#include "rendering/depth_prepass_system.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "rendering/deform_pre_pass.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_buffer.hpp"
#include "resources/ve_mesh.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"

namespace ve {

DepthPrePassSystem::DepthPrePassSystem(
	VeDevice& device,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& bindless_set_layout,
	vk::SampleCountFlagBits sample_count,
	vk::Format normal_roughness_format,
	std::filesystem::path shader_path,
	EventBus& event_bus)
	: m_ve_device(device), m_shader_path(std::move(shader_path)),
	  m_normal_roughness_format(normal_roughness_format) {

	event_bus.subscribe<PipelineRecreateEvent>([this](const PipelineRecreateEvent& e) {
		m_normal_roughness_format = e.offscreen_format;
		recreatePipeline(e.sample_count);
	});

	createPipelineLayout(global_set_layout, bindless_set_layout);
	createPipeline(sample_count);
}

DepthPrePassSystem::~DepthPrePassSystem() = default;

void DepthPrePassSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& bindless_set_layout) {
	// Set 1 (bindless) feeds the metallic-roughness texture sample in the G-buffer output
	vk::DescriptorSetLayout layouts[2] = {*global_set_layout, *bindless_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 2,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 0,
		.pPushConstantRanges = nullptr
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void DepthPrePassSystem::createPipeline(vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);

	pipeline_config.color_format = m_normal_roughness_format;
	// True would use roughness as blend factor
	pipeline_config.color_blend_attachment.blendEnable = VK_FALSE;
	// Only position + normal + uv (locations 0-2) are required
	auto attributes = VeMesh::Vertex::getAttributeDescriptions();
	attributes.resize(3);
	pipeline_config.attribute_descriptions = std::move(attributes);
	pipeline_config.binding_descriptions = VeMesh::Vertex::getBindingDescriptions();
	pipeline_config.multisample_info.rasterizationSamples = sample_count;
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eBack;
	pipeline_config.dynamic_state_enables.push_back(vk::DynamicState::eCullMode);
	pipeline_config.dynamic_state_info.dynamicStateCount =
		static_cast<uint32_t>(pipeline_config.dynamic_state_enables.size());
	pipeline_config.dynamic_state_info.pDynamicStates = pipeline_config.dynamic_state_enables.data();
	pipeline_config.depth_stencil_info.depthTestEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthWriteEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eGreater;
	pipeline_config.pipeline_layout = *m_pipeline_layout;

	m_ve_pipeline = std::make_unique<VePipeline>(m_ve_device, m_shader_path, pipeline_config);
}

void DepthPrePassSystem::render(
	VeFrameInfo& frame_info,
	PbrMegaBuffer& mega_buffer,
	const vk::raii::DescriptorSet& bindless_set,
	const VeBuffer& indirect_buffer,
	const uint32_t* bucket_offsets,
	const uint32_t* bucket_counts,
	uint32_t bucket_count) const {

	if (!mega_buffer.isValid())
		return;

	auto& cmd = frame_info.cmd();
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*frame_info.global_descriptor_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		1, {*bindless_set}, {});

	mega_buffer.bind(cmd);

	for (uint32_t bucket = 0; bucket < bucket_count; bucket++) {
		if (bucket_counts[bucket] == 0)
			continue;
		bool is_double_sided = (bucket & 1);
		cmd.setCullMode(is_double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);
		cmd.drawIndexedIndirect(
			indirect_buffer.getBuffer(),
			bucket_offsets[bucket] * sizeof(VkDrawIndexedIndirectCommand),
			bucket_counts[bucket],
			sizeof(VkDrawIndexedIndirectCommand));
	}
}

void DepthPrePassSystem::renderGpuCulled(
	VeFrameInfo& frame_info,
	PbrMegaBuffer& mega_buffer,
	const vk::raii::DescriptorSet& bindless_set,
	const VeBuffer& indirect_buffer,
	const uint32_t* bucket_group_offsets,
	const uint32_t* bucket_group_counts,
	uint32_t bucket_count,
	const VeBuffer* compacted_buffer,
	const VeBuffer* compact_count_buffer,
	const vk::raii::DescriptorSet* global_set_override) const {

	if (!mega_buffer.isValid())
		return;

	auto& cmd = frame_info.cmd();
	auto& global_set = global_set_override ? *global_set_override : frame_info.global_descriptor_set;
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*global_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		1, {*bindless_set}, {});

	mega_buffer.bind(cmd);

	for (uint32_t bucket = 0; bucket < bucket_count; bucket++) {
		if (bucket_group_counts[bucket] == 0)
			continue;
		bool is_double_sided = (bucket & 1);
		cmd.setCullMode(is_double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);
		auto offset = static_cast<vk::DeviceSize>(bucket_group_offsets[bucket]) * sizeof(VkDrawIndexedIndirectCommand);
		if (compacted_buffer && compact_count_buffer) {
			cmd.drawIndexedIndirectCount(
				compacted_buffer->getBuffer(), offset,
				compact_count_buffer->getBuffer(), bucket * sizeof(uint32_t),
				bucket_group_counts[bucket], sizeof(VkDrawIndexedIndirectCommand));
		} else {
			cmd.drawIndexedIndirect(
				indirect_buffer.getBuffer(), offset,
				bucket_group_counts[bucket], sizeof(VkDrawIndexedIndirectCommand));
		}
	}
}

void DepthPrePassSystem::renderGpuCulledMeshlets(
	VeFrameInfo& frame_info,
	PbrMegaBuffer& mega_buffer,
	const vk::raii::DescriptorSet& bindless_set,
	const VeBuffer& meshlet_indirect, const VeBuffer& draw_counts,
	const uint32_t* cpu_draw_counts,
	const vk::raii::DescriptorSet* global_set_override) const {

	if (!mega_buffer.hasMeshletData())
		return;

	auto& cmd = frame_info.cmd();
	auto& global_set = global_set_override ? *global_set_override : frame_info.global_descriptor_set;
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*global_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		1, {*bindless_set}, {});

	mega_buffer.bindMeshletIbo(cmd);

	// Depth prepass only draws non-mask buckets (0 = opaque-back, 1 = opaque-double-sided)
	for (uint32_t bucket = 0; bucket < 2; bucket++) {
		bool is_double_sided = (bucket & 1) != 0;
		cmd.setCullMode(is_double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);

		auto buf_offset = static_cast<vk::DeviceSize>(bucket) * MAX_MESHLET_DRAWS_PER_BUCKET
		                  * sizeof(VkDrawIndexedIndirectCommand);
		if (cpu_draw_counts) {
			uint32_t count = std::min(cpu_draw_counts[bucket], MAX_MESHLET_DRAWS_PER_BUCKET);
			cmd.drawIndexedIndirect(
				meshlet_indirect.getBuffer(), buf_offset,
				count, sizeof(VkDrawIndexedIndirectCommand));
		} else {
			auto count_offset = static_cast<vk::DeviceSize>(bucket) * sizeof(uint32_t);
			cmd.drawIndexedIndirectCount(
				meshlet_indirect.getBuffer(), buf_offset,
				draw_counts.getBuffer(), count_offset,
				MAX_MESHLET_DRAWS_PER_BUCKET, sizeof(VkDrawIndexedIndirectCommand));
		}
	}
}

void DepthPrePassSystem::recreatePipeline(vk::SampleCountFlagBits sample_count) {
	m_ve_pipeline.reset();
	createPipeline(sample_count);
}

} // namespace ve