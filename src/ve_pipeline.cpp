#include "pch.hpp"
#include "ve_pipeline.hpp"
#include "ve_device.hpp"
#include "ve_model.hpp"

#include <fstream>
#include <stdexcept>
#include <iostream>

namespace ve {
	VePipeline::VePipeline(
			VeDevice& ve_device,
			const char* shader_file_path,
			const PipelineConfigInfo& config_info) : ve_device(ve_device) {
		createGraphicsPipeline(shader_file_path, config_info);
	}

	VePipeline::~VePipeline() {}

	void VePipeline::defaultPipelineConfigInfo(PipelineConfigInfo& config_info) {

		// Tell the pipeline to expect dynamic viewport and scissor states
		config_info.dynamic_state_enables = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		config_info.dynamic_state_info = vk::PipelineDynamicStateCreateInfo{
			.dynamicStateCount = static_cast<uint32_t>(config_info.dynamic_state_enables.size()),
			.pDynamicStates = config_info.dynamic_state_enables.data()
		};
		config_info.viewport_info = { .viewportCount = 1, .scissorCount = 1 };

		config_info.input_assembly_info = {
			.sType = vk::StructureType::ePipelineInputAssemblyStateCreateInfo,
			.topology = vk::PrimitiveTopology::eTriangleList,
			.primitiveRestartEnable = VK_FALSE
		};
		config_info.rasterization_info = {
			.sType = vk::StructureType::ePipelineRasterizationStateCreateInfo,
			.depthClampEnable = VK_FALSE,
			.rasterizerDiscardEnable = VK_FALSE,
			.polygonMode = vk::PolygonMode::eFill,
			.cullMode = vk::CullModeFlagBits::eNone,
			.frontFace = vk::FrontFace::eClockwise,
			.depthBiasEnable = VK_TRUE,
			.lineWidth = 1.0f,
			.depthBiasConstantFactor = 0.0f,
			.depthBiasClamp = 0.0f,
			.depthBiasSlopeFactor = 0.0f
		};
		config_info.multisample_info = {
			.sType = vk::StructureType::ePipelineMultisampleStateCreateInfo,
			.sampleShadingEnable = VK_FALSE,
			.rasterizationSamples = vk::SampleCountFlagBits::e1,
			.minSampleShading = 1.0f,
			.pSampleMask = nullptr,
			.alphaToCoverageEnable = VK_FALSE,
			.alphaToOneEnable = VK_FALSE
		};
		config_info.depth_stencil_info = {
			.sType = vk::StructureType::ePipelineDepthStencilStateCreateInfo,
			.depthTestEnable = VK_TRUE,
			.depthWriteEnable = VK_TRUE,
			.depthCompareOp = vk::CompareOp::eLess,
			.depthBoundsTestEnable = VK_FALSE,
			.stencilTestEnable = VK_FALSE,
		};
		config_info.color_blend_attachment = {
			.blendEnable = VK_TRUE,
			.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
			.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
			.colorBlendOp = vk::BlendOp::eAdd,
			.srcAlphaBlendFactor = vk::BlendFactor::eOne,
			.dstAlphaBlendFactor = vk::BlendFactor::eZero,
			.alphaBlendOp = vk::BlendOp::eAdd,
			.colorWriteMask = vk::ColorComponentFlagBits::eR |
							  vk::ColorComponentFlagBits::eG |
							  vk::ColorComponentFlagBits::eB |
							  vk::ColorComponentFlagBits::eA
		};
		config_info.color_blend_info = {
			.sType = vk::StructureType::ePipelineColorBlendStateCreateInfo,
			.logicOpEnable = VK_FALSE,
			.logicOp = vk::LogicOp::eCopy,
			.attachmentCount = 1,
			.pAttachments = &config_info.color_blend_attachment
		};
		config_info.attribute_descriptions = VeModel::Vertex::getAttributeDescriptions();
		config_info.binding_descriptions = VeModel::Vertex::getBindingDescriptions();
	}

	std::vector<char> VePipeline::readFile(const char* file_path) {
		std::ifstream file(file_path, std::ios::ate | std::ios::binary);
		if (!file.is_open()) {
			throw std::runtime_error(std::string("failed to open file: ") + file_path);
		}

		size_t file_size = static_cast<size_t>(file.tellg());
		std::vector<char> buffer(file_size);

		file.seekg(0);
		file.read(buffer.data(), static_cast<std::streamsize>(file_size));
		file.close();
		return buffer;
	}

	void VePipeline::createGraphicsPipeline(
		const char* shader_file_path,
		const PipelineConfigInfo& config_info) {

		auto shader_code = readFile(shader_file_path);
		// Use the same combined SPIR-V for both stages; entry points differ per stage
		createShaderModule(shader_code, &shader_module);

		vk::PipelineShaderStageCreateInfo shader_stages[2] = {
			{
				.sType = vk::StructureType::ePipelineShaderStageCreateInfo,
				.stage = vk::ShaderStageFlagBits::eVertex,
				.module = *shader_module,
				.pName = "vertMain",
				.pSpecializationInfo = nullptr
			},
			{
				.sType = vk::StructureType::ePipelineShaderStageCreateInfo,
				.stage = vk::ShaderStageFlagBits::eFragment,
				.module = *shader_module,
				.pName = "fragMain",
				.pSpecializationInfo = nullptr
			}
		};
		vk::PipelineVertexInputStateCreateInfo vertex_input_info{
			.sType = vk::StructureType::ePipelineVertexInputStateCreateInfo,
			.vertexBindingDescriptionCount = static_cast<uint32_t>(config_info.binding_descriptions.size()),
			.pVertexBindingDescriptions = config_info.binding_descriptions.data(),
			.vertexAttributeDescriptionCount = static_cast<uint32_t>(config_info.attribute_descriptions.size()),
			.pVertexAttributeDescriptions = config_info.attribute_descriptions.data()
		};

		vk::Format depth_format = ve_device.findSupportedFormat(
			{vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
			vk::ImageTiling::eOptimal,
			vk::FormatFeatureFlagBits::eDepthStencilAttachment);
		vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &config_info.color_format,
			.depthAttachmentFormat = depth_format
		};
		vk::GraphicsPipelineCreateInfo pipeline_info{
			.sType = vk::StructureType::eGraphicsPipelineCreateInfo,
			.pNext = &pipelineRenderingCreateInfo,
			.stageCount = 2,
			.pStages = shader_stages,
			.pVertexInputState = &vertex_input_info,
			.pInputAssemblyState = &config_info.input_assembly_info,
			.pViewportState = &config_info.viewport_info,
			.pRasterizationState = &config_info.rasterization_info,
			.pMultisampleState = &config_info.multisample_info,
			.pColorBlendState = &config_info.color_blend_info,
			.pDynamicState = &config_info.dynamic_state_info,
			.pDepthStencilState = &config_info.depth_stencil_info,
			.layout = config_info.pipeline_layout,
			.renderPass = nullptr,
			.subpass = config_info.subpass,
			.basePipelineHandle = VK_NULL_HANDLE,
			.basePipelineIndex = -1
		};
		assert(config_info.pipeline_layout != nullptr && "Cannot create graphics pipeline: no pipelineLayout provided in config_info");

		VE_LOGD("Shader module code size: " << shader_code.size() << " bytes (shared for vert/frag)");

		graphics_pipeline = vk::raii::Pipeline{ve_device.getDevice(), nullptr, pipeline_info};
	}

	void VePipeline::createShaderModule(const std::vector<char>& code, vk::raii::ShaderModule* _shader_module) {
		vk::ShaderModuleCreateInfo create_info{
			.sType = vk::StructureType::eShaderModuleCreateInfo,
			.codeSize = code.size(),
			.pCode = reinterpret_cast<const uint32_t*>(code.data()) // leverage that vector already aligns data to worst-case alignment
		};

		try {
			*_shader_module = vk::raii::ShaderModule(ve_device.getDevice(), create_info);
		} catch (const std::exception& e) {
			throw std::runtime_error("failed to create shader module: " + std::string(e.what()));
		}
	}
}