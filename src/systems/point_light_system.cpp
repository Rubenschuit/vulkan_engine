#include "pch.hpp"
#include "point_light_system.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace ve {
	struct SimplePushConstantData {
		glm::vec4 position;
		glm::vec4 color;
		float scale;
	};

	PointLightSystem::PointLightSystem(
			VeDevice& device,
				const vk::raii::DescriptorSetLayout& global_set_layout,
				const vk::raii::DescriptorSetLayout& material_set_layout,
			vk::Format color_format) : m_ve_device(device) {

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
		VePipeline::defaultPipelineConfigInfo(pipeline_config);

		// set formats for dynamic rendering
		pipeline_config.color_format = color_format;
		pipeline_config.attribute_descriptions.clear();
		pipeline_config.binding_descriptions.clear();

		assert(m_pipeline_layout != nullptr && "Pipeline layout is null");
		pipeline_config.pipeline_layout = m_pipeline_layout;
		m_ve_pipeline = std::make_unique<VePipeline>(
			m_ve_device,
			"shaders/point_light_shader.spv",
			pipeline_config);
		assert(m_ve_pipeline != nullptr && "Failed to create pipeline");

	}

	void PointLightSystem::render(VeFrameInfo& frame_info) const {
		frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
		std::array<vk::DescriptorSet, 2> sets{*frame_info.global_descriptor_set, *frame_info.material_descriptor_set};
		frame_info.command_buffer.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics,
			*m_pipeline_layout,
			0,
			sets,
			{}
		);

		for (auto& [id, obj] : frame_info.game_objects) {
			if (obj.point_light_component == nullptr)
				continue;
			SimplePushConstantData push{};
			push.position = glm::vec4{obj.transform.translation, 1.0f};
			push.scale = obj.transform.scale.x;
			push.color = glm::vec4{obj.color, obj.point_light_component->intensity};
			frame_info.command_buffer.pushConstants<SimplePushConstantData>(
				*m_pipeline_layout,
				vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
				0,
				push
			);
			frame_info.command_buffer.draw(6, 1, 0, 0); // 6 vertices for point light
		}
	}

	// Update UBO with point light data for global access in shaders
	void PointLightSystem::update(VeFrameInfo& frame_info, UniformBufferObject& ubo) {
		uint32_t num_lights = 0;
		for (auto& [id, obj] : frame_info.game_objects) {
			if (obj.point_light_component == nullptr)
				continue;
			assert(num_lights <= MAX_LIGHTS && "Number of point lights exceeds MAX_LIGHTS");

			// rotate point lights in circle
			auto speed = 0.2f;
			auto rotate_matrix = glm::rotate(glm::mat4(1.0f), speed * frame_info.frame_time, glm::vec3(0.0f, 0.0f, 1.0f));
			auto pos = glm::vec4{obj.transform.translation, 1.0f};
			pos = rotate_matrix * pos;
			obj.transform.translation = glm::vec3{pos};

			ubo.point_lights[num_lights].position = glm::vec4{obj.transform.translation, 1.0f};
			ubo.point_lights[num_lights].color = glm::vec4{obj.color, obj.point_light_component->intensity};
			num_lights++;
		}

		ubo.num_lights = num_lights;
	}
}

