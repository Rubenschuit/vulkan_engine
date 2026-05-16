#include "pch.hpp"
#include "rendering/depth_prepass_system.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "rendering/skinning_pre_pass.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_buffer.hpp"
#include "resources/ve_mesh.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

namespace ve {

DepthPrePassSystem::DepthPrePassSystem(
	VeDevice& device,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	vk::SampleCountFlagBits sample_count,
	std::filesystem::path shader_path,
	EventBus& event_bus)
	: m_ve_device(device), m_shader_path(std::move(shader_path)) {

	event_bus.subscribe<PipelineRecreateEvent>([this](const PipelineRecreateEvent& e) {
		recreatePipeline(e.sample_count);
	});

	createPipelineLayout(global_set_layout);
	createPipeline(sample_count);
}

DepthPrePassSystem::~DepthPrePassSystem() = default;

void DepthPrePassSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout) {
	vk::DescriptorSetLayout layouts[1] = {*global_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 1,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 0,
		.pPushConstantRanges = nullptr
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void DepthPrePassSystem::createPipeline(vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);

	pipeline_config.color_format = vk::Format::eUndefined;
	pipeline_config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptionsShadow();
	pipeline_config.binding_descriptions = VeMesh::Vertex::getShadowBindingDescriptions();
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

	mega_buffer.bindShadow(cmd);

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

	mega_buffer.bindShadow(cmd);

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

void DepthPrePassSystem::renderSkinned(
	VeFrameInfo& frame_info,
	const std::vector<PbrRenderSystem::Drawable>& skinned_drawables) const {

	if (skinned_drawables.empty() || !frame_info.skinning_pre_pass)
		return;

	auto& cmd = frame_info.cmd();
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*frame_info.global_descriptor_set}, {});

	for (const auto& d : skinned_drawables) {
		if (!d.mesh_ptr)
			continue;
		VeBuffer* pos_buf = frame_info.skinning_pre_pass->getOutputPositionBuffer(
			d.entity, frame_info.current_frame);
		if (!pos_buf)
			continue;
		cmd.setCullMode(d.double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);
		vk::Buffer vbos[] = {pos_buf->getBuffer()};
		vk::DeviceSize offsets[] = {0};
		cmd.bindVertexBuffers(0, vbos, offsets);
		cmd.bindIndexBuffer(d.mesh_ptr->getIndexBuffer().getBuffer(), 0, vk::IndexType::eUint32);
		cmd.drawIndexed(d.mesh_ptr->getIndexCount(), 1, 0, 0, d.ssbo_index);
	}
}

void DepthPrePassSystem::renderGpuCulledMeshlets(
	VeFrameInfo& frame_info,
	PbrMegaBuffer& mega_buffer,
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

	mega_buffer.bindShadowMeshletIbo(cmd);

	// Depth prepass only draws non-mask buckets (0 = opaque-back, 1 = opaque-double-sided)
	constexpr uint32_t MAX_PER_BUCKET = MAX_MESHLET_DRAWS / MESHLET_BUCKET_COUNT;

	for (uint32_t bucket = 0; bucket < 2; bucket++) {
		bool is_double_sided = (bucket & 1) != 0;
		cmd.setCullMode(is_double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);

		auto buf_offset = static_cast<vk::DeviceSize>(bucket) * MAX_PER_BUCKET
		                  * sizeof(VkDrawIndexedIndirectCommand);
		if (cpu_draw_counts) {
			uint32_t count = std::min(cpu_draw_counts[bucket], MAX_PER_BUCKET);
			cmd.drawIndexedIndirect(
				meshlet_indirect.getBuffer(), buf_offset,
				count, sizeof(VkDrawIndexedIndirectCommand));
		} else {
			auto count_offset = static_cast<vk::DeviceSize>(bucket) * sizeof(uint32_t);
			cmd.drawIndexedIndirectCount(
				meshlet_indirect.getBuffer(), buf_offset,
				draw_counts.getBuffer(), count_offset,
				MAX_PER_BUCKET, sizeof(VkDrawIndexedIndirectCommand));
		}
	}
}

void DepthPrePassSystem::recreatePipeline(vk::SampleCountFlagBits sample_count) {
	m_ve_pipeline.reset();
	createPipeline(sample_count);
}

} // namespace ve