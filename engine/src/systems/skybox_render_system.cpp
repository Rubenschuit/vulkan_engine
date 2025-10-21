#include "pch.hpp"
#include "systems/skybox_render_system.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace ve {

struct SimplePushConstantData {
	glm::mat4 transform;
};

SkyboxRenderSystem::SkyboxRenderSystem( VeDevice& device,
										const vk::raii::DescriptorSetLayout& global_set_layout,
										const vk::raii::DescriptorSetLayout& material_set_layout,
										vk::Format color_format,
										std::filesystem::path shader_path,
										const std::filesystem::path& cube_model_path)
										: m_ve_device(device), m_shader_path(shader_path) {

	createPipelineLayout(global_set_layout, material_set_layout);
	createPipeline(color_format);
	loadCubeModel(cube_model_path);
}

SkyboxRenderSystem::~SkyboxRenderSystem() {}

void SkyboxRenderSystem::loadCubeModel(const std::filesystem::path& cube_model_path) {
	std::shared_ptr<VeModel> model = std::make_shared<VeModel>(m_ve_device, cube_model_path);
	m_cube_object.ve_model = model;
	m_cube_object.transform.scale = 4.0f * glm::vec3(1500.0f, 1500.0f, 1500.0f);
}

void SkyboxRenderSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex,
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

void SkyboxRenderSystem::createPipeline(vk::Format color_format) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config);

	// set formats for dynamic rendering
	pipeline_config.color_format = color_format;
	// Alter culling for skybox: only inside faces should be visible
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eBack;
	pipeline_config.rasterization_info.frontFace = vk::FrontFace::eClockwise;
	auto attribute_descriptions = VeModel::Vertex::getAttributeDescriptions();
	pipeline_config.attribute_descriptions = {attribute_descriptions[0]};

	assert(m_pipeline_layout != VK_NULL_HANDLE && "Pipeline layout is null");
	pipeline_config.pipeline_layout = m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config
	);
	assert(m_ve_pipeline && "Failed to create skybox pipeline");
}

// Performs a draw call for each game object with a model component
// TODO: bind and draw all objects with the same model at once
void SkyboxRenderSystem::render(VeFrameInfo& frame_info) {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		{},
		{*frame_info.global_descriptor_set, *frame_info.cubemap_descriptor_set},
		{}
	);
	SimplePushConstantData push{};
	assert (m_cube_object.ve_model != nullptr && "Cube model is null");
	float speed = 0.008f;
	m_cube_object.transform.rotation += glm::vec3{-speed * frame_info.frame_time, 0.2 * speed * frame_info.frame_time, 0.0f};

	push.transform =  m_cube_object.getTransform();
	frame_info.command_buffer.pushConstants<SimplePushConstantData>(
		*m_pipeline_layout,
		vk::ShaderStageFlagBits::eVertex,
		0,
		push
	);

	m_cube_object.ve_model->bindVertexBuffer(frame_info.command_buffer);
	m_cube_object.ve_model->bindIndexBuffer(frame_info.command_buffer);
	m_cube_object.ve_model->drawIndexed(frame_info.command_buffer);
}

}