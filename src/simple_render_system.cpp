#include "pch.hpp"
#include "simple_render_system.hpp"

namespace ve {
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
		vk::PipelineLayoutCreateInfo pipeline_layout_info{
			.sType = vk::StructureType::ePipelineLayoutCreateInfo,
			.setLayoutCount = 1,
			.pSetLayouts = &*descriptor_set_layout,
			.pushConstantRangeCount = 0,
			.pPushConstantRanges = nullptr
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

	void SimpleRenderSystem::drawFrame(VeFrameInfo& frame_info) const {
		frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, ve_pipeline->getPipeline());
		frame_info.ve_model.bindVertexBuffer(frame_info.command_buffer);
		frame_info.ve_model.bindIndexBuffer(frame_info.command_buffer);
		frame_info.command_buffer.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics,
			*pipeline_layout,
			0,
			*frame_info.global_descriptor_set,
			{}
		);
		frame_info.ve_model.drawIndexed(frame_info.command_buffer);
	}


}

