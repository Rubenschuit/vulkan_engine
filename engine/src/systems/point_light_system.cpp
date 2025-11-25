#include "pch.hpp"
#include "systems/point_light_system.hpp"
#include "core/ve_device.hpp"
#include "core/ve_pipeline.hpp"
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
									std::filesystem::path shader_path)
									: m_ve_device(device), m_shader_path(shader_path) {

	createPipelineLayout(global_set_layout, material_set_layout);
	createPipeline(color_format);
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

void PointLightSystem::createPipeline(vk::Format color_format) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);

	// set formats for dynamic rendering
	pipeline_config.color_format = color_format;
	pipeline_config.attribute_descriptions.clear();
	pipeline_config.binding_descriptions.clear();

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

	assert(m_pipeline_layout != VK_NULL_HANDLE && "Pipeline layout is null");
	pipeline_config.pipeline_layout = m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
	assert(m_ve_pipeline != VK_NULL_HANDLE && "Failed to create pipeline");

}


// Performs a draw call for each game object with a point light component
void PointLightSystem::render(VeFrameInfo& frame_info) const {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	std::array<vk::DescriptorSet, 2> sets{frame_info.global_descriptor_set, frame_info.texture_descriptor_set};
	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		sets,
		{}
	);

	for (auto& [id, obj] : frame_info.game_objects) {
		if (!obj.point_light_component)
			continue;
		SimplePushConstantData push{};
		push.position = glm::vec4{obj.transform.translation, 1.0f};
		push.scale = obj.transform.scale.x;
		push.color = glm::vec4{obj.color, obj.point_light_component->intensity};
		// push constant provided as raw bytes to avoid MSVC debug mode corruption with push across dll boundaries
		frame_info.command_buffer.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		frame_info.command_buffer.draw(6, 1, 0, 0); // 6 vertices for point light
	}
}

// Update UBO with point light data for global access in shaders
void PointLightSystem::updateUniformBuffer(VeFrameInfo& frame_info, UniformBufferObject& ubo) {
	// Single pass: populate both point_lights and shadow_lights arrays
	uint32_t num_lights = 0;
	uint32_t num_shadow_lights = 0;

	for (auto& [id, obj] : frame_info.game_objects) {
		if (!obj.point_light_component)
			continue;
		assert(num_lights < MAX_LIGHTS && "Number of point lights exceeds MAX_LIGHTS");

		// Rotate point lights in circle
		if (obj.point_light_component->rotates) {
			auto speed = 0.04f;
			auto rotate_matrix = glm::rotate(glm::mat4(1.0f), speed * frame_info.frame_time, glm::vec3(0.0f, 0.0f, 1.0f));
			auto pos = glm::vec4{obj.transform.translation, 1.0f};
			pos = rotate_matrix * pos;
			obj.transform.translation = glm::vec3{pos};
		}

		// Populate point light data
		ubo.point_lights[num_lights].position = glm::vec4{obj.transform.translation, 1.0f};
		ubo.point_lights[num_lights].color = glm::vec4{obj.color, obj.point_light_component->intensity};

		// If this light casts shadows, add it to shadow_lights array
		if (obj.point_light_component->casts_shadow && num_shadow_lights < MAX_SHADOW_LIGHTS) {
			glm::vec3 light_pos = obj.transform.translation;
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
	static bool first_log = true;
	if (first_log) {
		first_log = false;
		VE_LOGI("Point light system: updated UBO with " << num_lights << " lights, " << num_shadow_lights << " shadow-casting");
	}
}
} // namespace ve