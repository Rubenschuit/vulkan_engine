#include "pch.hpp"
#include "simple_render_system.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace ve {

	/* Scalars have to be aligned by N (= 4 bytes given 32-bit floats).
	   A float2 must be aligned by 2N (= 8 bytes).
	   A float3 or float4 must be aligned by 4N (= 16 bytes).
	   A nested structure must be aligned by the base alignment of its members rounded up to a multiple of 16.
	   A float4x4 matrix must have the same alignment as a float4.    */
	// TODO: currently exceeds max push constant size of 128 bytes on some hardware
	struct SimplePushConstantData {
		glm::mat4 transform;
		glm::mat4 normal_transform;
		alignas(4) float has_texture;
	};

	SimpleRenderSystem::SimpleRenderSystem(
			VeDevice& device,
				const vk::raii::DescriptorSetLayout& global_set_layout,
				const vk::raii::DescriptorSetLayout& material_set_layout,
			vk::Format color_format) : m_ve_device(device) {

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

		// set formats for dynamic rendering
		pipeline_config.color_format = color_format;

		assert(m_pipeline_layout != VK_NULL_HANDLE && "Pipeline layout is null");
		pipeline_config.pipeline_layout = m_pipeline_layout;
		m_ve_pipeline = std::make_unique<VePipeline>(
			m_ve_device,
			"shaders/simple_shader.spv",
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
			push.normal_transform = obj.getNormalTransform();
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


}

