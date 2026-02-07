#include "pch.hpp"
#include "systems/skybox_render_system.hpp"
#include "core/ve_device.hpp"
#include "core/ve_pipeline.hpp"
#include "game/ve_component.hpp"
#include "utils/ve_log.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

namespace ve {

struct SimplePushConstantData {
	alignas(16) glm::mat4 transform;
};
static_assert(sizeof(SimplePushConstantData) == 64, "SimplePushConstantData size mismatch");
static_assert(offsetof(SimplePushConstantData, transform) == 0, "SimplePushConstantData transform offset mismatch");

SkyboxRenderSystem::SkyboxRenderSystem(
	VeDevice& device,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	vk::Format color_format,
	vk::SampleCountFlagBits sample_count,
	std::filesystem::path shader_path,
	const std::filesystem::path& cube_model_path)
	: m_ve_device(device), m_shader_path(shader_path) {

	createPipelineLayout(global_set_layout, material_set_layout);
	createPipeline(color_format, sample_count);
	loadCubeModel(cube_model_path);
}

SkyboxRenderSystem::~SkyboxRenderSystem() {}

void SkyboxRenderSystem::loadCubeModel(const std::filesystem::path& cube_model_path) {
	std::shared_ptr<VeModel> model = std::make_shared<VeModel>(m_ve_device, cube_model_path);
	m_cube_object.addComponent<ModelComponent>(model);
	auto* transform = m_cube_object.getComponent<TransformComponent>();
	transform->scale = 4.0f * glm::vec3(1500.0f, 1500.0f, 1500.0f);
}
void SkyboxRenderSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout) {

	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex,
		.offset = 0, // Used for indexing multiple push constant ranges
		.size = sizeof(SimplePushConstantData)
	};
	// Debug: Verify push constant size
	VE_LOGD("Skybox push constant size: " << sizeof(SimplePushConstantData) << " bytes");
	// Store raw handles to avoid DLL boundary issues with RAII objects
	vk::DescriptorSetLayout layouts[2] = {*global_set_layout, *material_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 2,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void SkyboxRenderSystem::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);
	pipeline_config.multisample_info.rasterizationSamples = sample_count;

	// set formats for dynamic rendering
	pipeline_config.color_format = color_format;
	// Alter culling for skybox: only inside faces need to be visible
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eFront;
	pipeline_config.depth_stencil_info.depthWriteEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eLessOrEqual;
	auto attribute_descriptions = VeModel::Vertex::getAttributeDescriptionsSimple();
	pipeline_config.attribute_descriptions = {attribute_descriptions[0]};

	pipeline_config.pipeline_layout = *m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config
	);
	assert(m_ve_pipeline && "Failed to create skybox pipeline");
}

// Draws a big cube and binds cubemap texture for shader
// TODO: move update logic to a separate function
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
	auto* model = m_cube_object.getComponent<ModelComponent>();
	auto* transform = m_cube_object.getComponent<TransformComponent>();
	assert(model && model->hasModel() && transform && "Cube must have Model and Transform components");
	float speed = 0.004f;
	transform->rotation += glm::vec3{-speed * frame_info.frame_time, 0.2 * speed * frame_info.frame_time, 0.0f};

	push.transform = transform->getTransform();

	frame_info.command_buffer.pushConstants(
		*m_pipeline_layout,
		vk::ShaderStageFlagBits::eVertex,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
	);

	model->model->bindVertexBuffer(frame_info.command_buffer);
	model->model->bindIndexBuffer(frame_info.command_buffer);
	model->model->drawIndexed(frame_info.command_buffer);
}

}