#include "pch.hpp"
#include "systems/simple_render_system.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace ve {

struct SimplePushConstantData {
	alignas(16) glm::mat4 transform;            
	alignas(16) glm::mat3x4 normal_transform;   
	alignas(4)  float has_texture;  
	alignas(4)  float padding[3];	       
};
static_assert(sizeof(SimplePushConstantData) <= 128, "Push constants must be 128 bytes for stable layout");

SimpleRenderSystem::SimpleRenderSystem( VeDevice& device,
										const vk::raii::DescriptorSetLayout& global_set_layout,
										const vk::raii::DescriptorSetLayout& material_set_layout,
										vk::Format color_format,
										std::filesystem::path shader_path)
										: m_ve_device(device), m_shader_path(shader_path) {

	createPipelineLayout(global_set_layout, material_set_layout);
	createPipeline(color_format);

}

SimpleRenderSystem::~SimpleRenderSystem() {
}

void SimpleRenderSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0, // Used for indexing multiple push constant ranges
		.size = sizeof(SimplePushConstantData)
	};
	std::array<vk::DescriptorSetLayout, 2> set_layouts{*global_set_layout, *material_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void SimpleRenderSystem::createPipeline(vk::Format color_format) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config);
	pipeline_config.color_format = color_format;

	assert(m_pipeline_layout != VK_NULL_HANDLE && "Pipeline layout is null");
	pipeline_config.pipeline_layout = m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
	assert(m_ve_pipeline != VK_NULL_HANDLE && "Failed to create pipeline");

}

// Performs a draw call for each game object with a model component
// TODO: bind and draw all objects with the same model at once
void SimpleRenderSystem::renderObjects(VeFrameInfo& frame_info) const {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		{},
		{*frame_info.global_descriptor_set, *frame_info.material_descriptor_set},
		{}
	);

	for (auto& [id, obj] : frame_info.game_objects) {
		// Skip non-mesh objects (e.g., point lights) or missing models
		if (!obj.ve_model) continue;
		SimplePushConstantData push{};
		// Pack glm::mat3 into 3 vec4 columns (last component is padding)
		const glm::mat3 nrm = obj.getNormalTransform();
		push.normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		push.normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		push.normal_transform[2] = glm::vec4(nrm[2], 0.0f);
		push.transform = obj.getTransform();
		push.has_texture = obj.has_texture;
		frame_info.command_buffer.pushConstants<SimplePushConstantData>(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			push
		);
		obj.ve_model->bindVertexBuffer(frame_info.command_buffer);
		obj.ve_model->bindIndexBuffer(frame_info.command_buffer);
		obj.ve_model->drawIndexed(frame_info.command_buffer);
	}
}

} // namespace ve
