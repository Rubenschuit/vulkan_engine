#include "pch.hpp"
#include "core/ve_pipeline.hpp"
#include "core/ve_device.hpp"
#include "game/ve_model.hpp"
#include "core/ve_file_system.hpp"

namespace ve {

VePipeline::VePipeline(
		VeDevice& ve_device,
		const std::filesystem::path& shader_file_path,
		const PipelineConfigInfo& config_info) : m_ve_device(ve_device) {
	createGraphicsPipeline(shader_file_path, config_info);
}

VePipeline::~VePipeline() {}

void VePipeline::defaultPipelineConfigInfo(PipelineConfigInfo& config_info, VeDevice& ve_device) {

	// Tell the pipeline to expect dynamic viewport and scissor states
	config_info.dynamic_state_enables = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	config_info.dynamic_state_info = vk::PipelineDynamicStateCreateInfo{
		.sType = vk::StructureType::ePipelineDynamicStateCreateInfo,
		.pNext = nullptr,
		.flags = {},
		.dynamicStateCount = static_cast<uint32_t>(config_info.dynamic_state_enables.size()),
		.pDynamicStates = config_info.dynamic_state_enables.data()
	};
	config_info.viewport_info = { .sType = vk::StructureType::ePipelineViewportStateCreateInfo, .pNext = nullptr, .flags = {}, .viewportCount = 1, .pViewports = nullptr, .scissorCount = 1, .pScissors = nullptr };

	config_info.input_assembly_info = {
		.sType = vk::StructureType::ePipelineInputAssemblyStateCreateInfo,
		.pNext = nullptr,
		.flags = {},
		.topology = vk::PrimitiveTopology::eTriangleList,
		.primitiveRestartEnable = VK_FALSE
	};
	config_info.rasterization_info = {
		.sType = vk::StructureType::ePipelineRasterizationStateCreateInfo,
		.pNext = nullptr,
		.flags = {},
		.depthClampEnable = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE,
		.polygonMode = vk::PolygonMode::eFill,
		.cullMode = vk::CullModeFlagBits::eBack,
		.frontFace = vk::FrontFace::eClockwise,
		.depthBiasEnable = VK_FALSE,
		.depthBiasConstantFactor = 0.0f,
		.depthBiasClamp = 0.0f,
		.depthBiasSlopeFactor = 0.0f,
		.lineWidth = 1.0f
	};
	config_info.multisample_info = {
		.sType = vk::StructureType::ePipelineMultisampleStateCreateInfo,
		.pNext = nullptr,
		.flags = {},
		.rasterizationSamples = ve_device.getSampleCount(),
		.sampleShadingEnable = VK_FALSE,
		.minSampleShading = 1.0f,
		.pSampleMask = nullptr,
		.alphaToCoverageEnable = VK_FALSE,
		.alphaToOneEnable = VK_FALSE
	};
	config_info.depth_stencil_info = {
		.sType = vk::StructureType::ePipelineDepthStencilStateCreateInfo,
		.pNext = nullptr,
		.flags = {},
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = vk::CompareOp::eLess,
		.depthBoundsTestEnable = VK_FALSE,
		.stencilTestEnable = VK_FALSE,
		.front = {},
		.back = {},
		.minDepthBounds = 0.0f,
		.maxDepthBounds = 1.0f
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
		.pNext = nullptr,
		.flags = {},
		.logicOpEnable = VK_FALSE,
		.logicOp = vk::LogicOp::eCopy,
		.attachmentCount = 1,
		.pAttachments = &config_info.color_blend_attachment
	};
	config_info.attribute_descriptions = VeModel::Vertex::getAttributeDescriptionsSimple();
	config_info.binding_descriptions = VeModel::Vertex::getBindingDescriptions();
}

void VePipeline::createGraphicsPipeline(
		const std::filesystem::path& shader_file_path,
		const PipelineConfigInfo& config_info) {

	auto shader_code = VeFileSystem::readFile(shader_file_path);
	// Use the same combined SPIR-V for both stages; entry points differ per stage
	createShaderModule(shader_code, &m_shader_module);

	// Build specialization constant data if provided
	std::vector<vk::SpecializationMapEntry> spec_map_entries;
	std::vector<uint32_t> spec_data;
	vk::SpecializationInfo frag_spec_info{};

	if (!config_info.specialization_constants.empty()) {
		spec_map_entries.reserve(config_info.specialization_constants.size());
		spec_data.reserve(config_info.specialization_constants.size());

		uint32_t offset = 0;
		for (const auto& [constant_id, value] : config_info.specialization_constants) {
			spec_map_entries.push_back({
				.constantID = constant_id,
				.offset = static_cast<uint32_t>(offset * sizeof(uint32_t)),
				.size = sizeof(uint32_t)
			});
			spec_data.push_back(value);
			offset++;
		}

		frag_spec_info = {
			.mapEntryCount = static_cast<uint32_t>(spec_map_entries.size()),
			.pMapEntries = spec_map_entries.data(),
			.dataSize = spec_data.size() * sizeof(uint32_t),
			.pData = spec_data.data()
		};
	}

	vk::PipelineShaderStageCreateInfo shader_stages[2] = {
		{
			.sType = vk::StructureType::ePipelineShaderStageCreateInfo,
			.stage = vk::ShaderStageFlagBits::eVertex,
			.module = *m_shader_module,
			.pName = "vertMain",
			.pSpecializationInfo = nullptr
		},
		{
			.sType = vk::StructureType::ePipelineShaderStageCreateInfo,
			.stage = vk::ShaderStageFlagBits::eFragment,
			.module = *m_shader_module,
			.pName = "fragMain",
			.pSpecializationInfo = config_info.specialization_constants.empty() ? nullptr : &frag_spec_info
		}
	};
	vk::PipelineVertexInputStateCreateInfo vertex_input_info{
		.sType = vk::StructureType::ePipelineVertexInputStateCreateInfo,
		.vertexBindingDescriptionCount = static_cast<uint32_t>(config_info.binding_descriptions.size()),
		.pVertexBindingDescriptions = config_info.binding_descriptions.data(),
		.vertexAttributeDescriptionCount = static_cast<uint32_t>(config_info.attribute_descriptions.size()),
		.pVertexAttributeDescriptions = config_info.attribute_descriptions.data()
	};
	// Use provided depth format, or query for one if not specified
	vk::Format depth_format = (config_info.depth_format != vk::Format::eUndefined)
		? config_info.depth_format
		: m_ve_device.findSupportedFormat(
			{vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
			vk::ImageTiling::eOptimal,
			vk::FormatFeatureFlagBits::eDepthStencilAttachment
		);
	uint32_t color_attachment_count = (config_info.color_format == vk::Format::eUndefined) ? 0U : 1U;
	vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{
		.sType = vk::StructureType::ePipelineRenderingCreateInfo,
		.pNext = nullptr,
		.viewMask = 0,
		.colorAttachmentCount = color_attachment_count,
		.pColorAttachmentFormats = (config_info.color_format == vk::Format::eUndefined) ? nullptr : &config_info.color_format,
		.depthAttachmentFormat = depth_format,
		.stencilAttachmentFormat = vk::Format::eUndefined
	};
	vk::GraphicsPipelineCreateInfo pipeline_info{
		.sType = vk::StructureType::eGraphicsPipelineCreateInfo,
		.pNext = &pipelineRenderingCreateInfo,
		.flags = {},
		.stageCount = 2,
		.pStages = shader_stages,
		.pVertexInputState = &vertex_input_info,
		.pInputAssemblyState = &config_info.input_assembly_info,
		.pTessellationState = nullptr,
		.pViewportState = &config_info.viewport_info,
		.pRasterizationState = &config_info.rasterization_info,
		.pMultisampleState = &config_info.multisample_info,
		.pDepthStencilState = &config_info.depth_stencil_info,
		.pColorBlendState = &config_info.color_blend_info,
		.pDynamicState = &config_info.dynamic_state_info,
		.layout = config_info.pipeline_layout,
		.renderPass = nullptr, // Using dynamic rendering
		.subpass = 0,
		.basePipelineHandle = VK_NULL_HANDLE,
		.basePipelineIndex = -1
	};
	assert(config_info.pipeline_layout != VK_NULL_HANDLE && "Cannot create graphics pipeline: no pipelineLayout provided in config_info");

	m_graphics_pipeline = vk::raii::Pipeline{m_ve_device.getDevice(), nullptr, pipeline_info};
}

void VePipeline::createShaderModule(const std::vector<char>& code, vk::raii::ShaderModule* _shader_module) {
	vk::ShaderModuleCreateInfo create_info{
		.sType = vk::StructureType::eShaderModuleCreateInfo,
		.codeSize = code.size(),
		.pCode = reinterpret_cast<const uint32_t*>(code.data()) // leverage that vector already aligns data to worst-case alignment
	};

	*_shader_module = vk::raii::ShaderModule(m_ve_device.getDevice(), create_info);
}
}