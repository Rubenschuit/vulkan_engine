#include "pch.hpp"
#include "rendering/point_light_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "utils/ve_log.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace ve {

//TODO put scale in pos.w
struct SimplePushConstantData {
	glm::vec4 position;
	glm::vec4 color;
	float scale;
	uint32_t padding[3];
};

PointLightSystem::PointLightSystem( VeDevice& device,
									const vk::raii::DescriptorSetLayout& global_set_layout,
									const vk::raii::DescriptorSetLayout& material_set_layout,
									vk::Format color_format,
									vk::SampleCountFlagBits sample_count,
									std::filesystem::path shader_path)
									: m_ve_device(device), m_shader_path(shader_path) {

	createPipelineLayout(global_set_layout, material_set_layout);
	createPipeline(color_format, sample_count);
}

PointLightSystem::~PointLightSystem() {
}

void PointLightSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout) {
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

void PointLightSystem::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);
	pipeline_config.multisample_info.rasterizationSamples = sample_count;

	// set formats for dynamic rendering
	pipeline_config.color_format = color_format;
	pipeline_config.attribute_descriptions.clear();
	pipeline_config.binding_descriptions.clear();

	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	pipeline_config	.depth_stencil_info.depthTestEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eLessOrEqual;
	pipeline_config.rasterization_info.depthBiasEnable = VK_TRUE;
	pipeline_config.rasterization_info.depthBiasConstantFactor = 0.0f;
	pipeline_config.rasterization_info.depthBiasClamp = 0.0f;
	pipeline_config.rasterization_info.depthBiasSlopeFactor = -1.0f;

	//enable additve blending
	pipeline_config.color_blend_attachment.blendEnable = VK_TRUE;
	pipeline_config.color_blend_attachment.srcColorBlendFactor = vk::BlendFactor::eOne; //
	pipeline_config.color_blend_attachment.dstColorBlendFactor = vk::BlendFactor::eOne; //
	pipeline_config.color_blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
	pipeline_config.color_blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne; //
	pipeline_config.color_blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne; //
	pipeline_config.color_blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;

	pipeline_config.pipeline_layout = *m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
	assert(m_ve_pipeline != VK_NULL_HANDLE && "Failed to create pipeline");

}


// Performs a draw call for each point light, iterating the dense pool directly
void PointLightSystem::render(VeFrameInfo& frame_info) const {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	std::array<vk::DescriptorSet, 2> sets{*frame_info.global_descriptor_set, *frame_info.texture_descriptor_set};
	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		sets,
		{}
	);

	auto& registry = *frame_info.registry;
	auto& pl_pool = registry.pointLights();
	for (uint32_t i = 0; i < pl_pool.size(); i++) {
		uint32_t entity_idx = pl_pool.entityAt(i);
		Entity entity = registry.entityFromIndex(entity_idx);
		if (!registry.isActive(entity)) continue;
		auto* transform = registry.getComponent<TransformComponent>(entity);
		if (!transform) continue;
		PointLightComponent& pl = pl_pool.data()[i];
		SimplePushConstantData push{};
		push.position = glm::vec4{transform->getTranslation(), 1.0f};
		push.scale = transform->getScale().x;
		push.color = glm::vec4{pl.color, pl.intensity};
		frame_info.command_buffer.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		frame_info.command_buffer.draw(6, 1, 0, 0);
	}
}

// Update UBO with point light data
void PointLightSystem::updateUniformBuffer(VeFrameInfo& frame_info, UniformBufferObject& ubo) {
	uint32_t num_lights = 0;
	uint32_t num_shadow_lights = 0;

	// Pre-multiply ambient light color by intensity
	ubo.ambient_light_color.x *= ubo.ambient_light_color.w;
	ubo.ambient_light_color.y *= ubo.ambient_light_color.w;
	ubo.ambient_light_color.z *= ubo.ambient_light_color.w;

	auto& registry = *frame_info.registry;
	auto& pl_pool = registry.pointLights();
	for (uint32_t i = 0; i < pl_pool.size(); i++) {
		uint32_t entity_idx = pl_pool.entityAt(i);
		Entity entity = registry.entityFromIndex(entity_idx);
		if (!registry.isActive(entity))
			continue;
		auto* transform = registry.getComponent<TransformComponent>(entity);
		if (!transform)
			continue;
		assert(num_lights < MAX_LIGHTS && "Number of point lights exceeds MAX_LIGHTS");

		PointLightComponent& pl = pl_pool.data()[i];
		glm::vec3 color = pl.color;

		ubo.point_lights[num_lights].position = glm::vec4{transform->getTranslation(), 1.0f};
		ubo.point_lights[num_lights].color.x = color.x * pl.intensity;
		ubo.point_lights[num_lights].color.y = color.y * pl.intensity;
		ubo.point_lights[num_lights].color.z = color.z * pl.intensity;
		ubo.point_lights[num_lights].color.w = pl.intensity;

		if (pl.casts_shadow && num_shadow_lights < MAX_SHADOW_LIGHTS) {
			glm::vec3 light_pos = transform->getTranslation();
			glm::vec3 scene_center = glm::vec3(0.0f, 0.0f, 0.0f);
			glm::vec3 view_up = glm::vec3(0.0f, 1.0f, 0.0f);
			glm::mat4 light_view = glm::lookAt(light_pos, scene_center, view_up);
			float near_plane = 1.0f;
			float far_plane = 400.0f;
			glm::mat4 light_proj = glm::perspective(glm::radians(100.0f), 1.0f, near_plane, far_plane);

			ubo.shadow_lights[num_shadow_lights].light_index_padding.x = static_cast<float>(num_lights);
			ubo.shadow_lights[num_shadow_lights].light_view = light_view;
			ubo.shadow_lights[num_shadow_lights].light_proj = light_proj;
			num_shadow_lights++;
		}

		num_lights++;
	}

	ubo.num_lights = num_lights;
	ubo.num_shadow_lights = num_shadow_lights;
}
} // namespace ve