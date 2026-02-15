#include "pch.hpp"
#include "rendering/depth_prepass_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "resources/ve_mesh.hpp"
#include "utils/ve_log.hpp"

namespace ve {

struct DepthPrePassPushConstant {
	alignas(4) uint32_t instance_offset;
};
static_assert(sizeof(DepthPrePassPushConstant) == 4);

DepthPrePassSystem::DepthPrePassSystem(
	VeDevice& device,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	vk::SampleCountFlagBits sample_count,
	std::filesystem::path shader_path)
	: m_ve_device(device), m_shader_path(std::move(shader_path)) {
	createPipelineLayout(global_set_layout);
	createPipeline(sample_count);
}

DepthPrePassSystem::~DepthPrePassSystem() = default;

void DepthPrePassSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex,
		.offset = 0,
		.size = sizeof(DepthPrePassPushConstant)
	};

	vk::DescriptorSetLayout layouts[1] = {*global_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 1,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void DepthPrePassSystem::createPipeline(vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);

	// Depth-only: no color attachment
	pipeline_config.color_format = vk::Format::eUndefined;

	// Position-only vertex layout (same as shadow pass)
	pipeline_config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptionsShadow();
	pipeline_config.binding_descriptions = VeMesh::Vertex::getShadowBindingDescriptions();

	// Match scene MSAA sample count (unlike shadow maps which are always 1x)
	pipeline_config.multisample_info.rasterizationSamples = sample_count;

	// Depth bias: push pre-pass depth slightly further so the main pass eLess test
	// passes for the same geometry (true_depth < biased_depth).
	pipeline_config.rasterization_info.depthBiasEnable = VK_TRUE;
	pipeline_config.rasterization_info.depthBiasConstantFactor = 2.0f;
	pipeline_config.rasterization_info.depthBiasSlopeFactor = 0.0f;
	pipeline_config.rasterization_info.depthBiasClamp = 0.0f;

	// Back-face culling default, with dynamic override for double-sided
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eBack;
	pipeline_config.dynamic_state_enables.push_back(vk::DynamicState::eCullMode);
	pipeline_config.dynamic_state_info.dynamicStateCount =
		static_cast<uint32_t>(pipeline_config.dynamic_state_enables.size());
	pipeline_config.dynamic_state_info.pDynamicStates = pipeline_config.dynamic_state_enables.data();

	// Standard depth test + write
	pipeline_config.depth_stencil_info.depthTestEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthWriteEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eLess;

	pipeline_config.pipeline_layout = *m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
}

void DepthPrePassSystem::render(
	VeFrameInfo& frame_info,
	const std::vector<PbrRenderSystem::InstanceGroup>& opaque_groups) const {

	auto& cmd = frame_info.command_buffer;

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());

	// Bind global descriptor set (set 0): UBO + instance SSBO
	cmd.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*frame_info.global_descriptor_set}, {});

	VeMesh* bound_mesh = nullptr;

	for (const auto& group : opaque_groups) {
		// Skip alpha-masked groups: depth-only shader cannot do alpha test
		if (group.alpha_cutoff > 0.0f)
			continue;

		// Dynamic cull mode for double-sided geometry
		if (group.double_sided)
			cmd.setCullMode(vk::CullModeFlagBits::eNone);
		else
			cmd.setCullMode(vk::CullModeFlagBits::eBack);

		// Push instance_offset for SSBO indexing
		DepthPrePassPushConstant push{
			.instance_offset = group.first_instance
		};
		cmd.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(push), reinterpret_cast<const uint8_t*>(&push))
		);

		// Bind shadow VBO + IBO (position-only, if mesh changed)
		if (group.mesh != bound_mesh) {
			bound_mesh = group.mesh;
			bound_mesh->bindShadowVertexBuffer(cmd);
			bound_mesh->bindIndexBuffer(cmd);
		}

		// Instanced draw
		group.mesh->drawIndexed(cmd, group.instance_count, 0);
	}
}

} // namespace ve
