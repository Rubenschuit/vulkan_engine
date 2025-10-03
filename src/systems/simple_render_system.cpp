#include "pch.hpp"
#include "simple_render_system.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace ve {

	struct SimplePushConstantData {
		alignas(16) glm::vec3 offset{0.0f};
		alignas(16) glm::vec3 scale{1.0f};
	};

	SimpleRenderSystem::SimpleRenderSystem(
			VeDevice& device,
			vk::raii::DescriptorSetLayout& descriptor_set_layout,
			vk::Format color_format) : ve_device(device) {

		createPipelineLayout(descriptor_set_layout);
		createPipeline(color_format);
	}

	SimpleRenderSystem::~SimpleRenderSystem() {
	}

	void SimpleRenderSystem::createPipelineLayout(vk::raii::DescriptorSetLayout& descriptor_set_layout) {
		vk::PushConstantRange push_constant_range{
			.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			.offset = 0, // Used for indexing multiple push constant ranges
			.size = sizeof(SimplePushConstantData)
		};
		vk::PipelineLayoutCreateInfo pipeline_layout_info{
			.sType = vk::StructureType::ePipelineLayoutCreateInfo,
			.setLayoutCount = 1,
			.pSetLayouts = &*descriptor_set_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant_range
		};
		pipeline_layout = vk::raii::PipelineLayout(ve_device.getDevice(), pipeline_layout_info);
	}

	void SimpleRenderSystem::createPipeline(vk::Format color_format) {
		PipelineConfigInfo pipeline_config{};
		VePipeline::defaultPipelineConfigInfo(pipeline_config);

		// set formats for dynamic rendering
		pipeline_config.color_format = color_format;

		assert(pipeline_layout != nullptr && "Pipeline layout is null");
		pipeline_config.pipeline_layout = pipeline_layout;
		ve_pipeline = std::make_unique<VePipeline>(
			ve_device,
			"../shaders/simple_shader.spv",
			pipeline_config);
		assert(ve_pipeline != nullptr && "Failed to create pipeline");

	}

	void SimpleRenderSystem::renderObjects(VeFrameInfo& frame_info) const {
		frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, ve_pipeline->getPipeline());
		frame_info.command_buffer.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics,
			*pipeline_layout,
			0,
			*frame_info.global_descriptor_set,
			{}
		);

		for (auto& [id, obj] : frame_info.game_objects) {
			SimplePushConstantData push{};
			push.offset = obj.position;
			push.scale = obj.scale;
			frame_info.command_buffer.pushConstants<SimplePushConstantData>(
				*pipeline_layout,
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

