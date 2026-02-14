#include "pch.hpp"
#include "rendering/simple_render_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"
#include "utils/ve_log.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <algorithm>
#include <vector>

namespace ve {

struct SimplePushConstantData {
	alignas(16) glm::mat4 transform;
	alignas(16) glm::mat3x4 normal_transform; // 3x4 matrix for allignment (could be done better)
	alignas(4)  float has_texture;
	alignas(4)  float padding[3];
};
static_assert(sizeof(SimplePushConstantData) <= 128, "Push constants must be 128 bytes for stable layout");

SimpleRenderSystem::SimpleRenderSystem(
	VeDevice& device,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout,
	vk::Format color_format,
	vk::SampleCountFlagBits sample_count,
	std::filesystem::path shader_path)
	: m_ve_device(device), m_shader_path(shader_path), m_color_format(color_format), m_sample_count(sample_count) {

	createPipelineLayout(global_set_layout, material_set_layout, shadow_set_layout);
	createPipeline(m_color_format, m_sample_count);
}

SimpleRenderSystem::~SimpleRenderSystem() {
}

void SimpleRenderSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout, const vk::raii::DescriptorSetLayout& shadow_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0, // Used for indexing multiple push constant ranges
		.size = sizeof(SimplePushConstantData)
	};
	vk::DescriptorSetLayout layouts[3] = {*global_set_layout, *material_set_layout, *shadow_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 3,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void SimpleRenderSystem::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);
	pipeline_config.multisample_info.rasterizationSamples = sample_count;
	pipeline_config.color_format = color_format;
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eFront;
	pipeline_config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptionsSimple();
	pipeline_config.input_assembly_info.topology = m_topology;

	pipeline_config.pipeline_layout = *m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
	assert(m_ve_pipeline != VK_NULL_HANDLE && "Failed to create pipeline");

}

// Renders visible game objects from frame_info.visible_game_objects. The objects are sorted by material set
// to reduce descriptor set changes.
void SimpleRenderSystem::renderObjects(VeFrameInfo& frame_info) const {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());

	struct Drawable {
		VkDescriptorSet material_set;
		VeGameObject* obj;
		MeshComponent* mesh = nullptr;
	};
	std::vector<Drawable> drawables;
	drawables.reserve(frame_info.visible_game_objects.size());
	for (auto& [id, entry] : frame_info.visible_game_objects) {
		VeGameObject& obj = *entry.obj;
		MeshComponent* mesh = entry.mesh;
		if (!mesh || !mesh->getMesh() || !obj.getComponent<TransformComponent>())
			continue;
		if (!mesh->getMaterial())
			continue;
		auto* mat = mesh->getMaterial();
		vk::raii::DescriptorSet& mat_set = mat->hasDescriptorSet()
			? mat->getDescriptorSet()
			: frame_info.material_descriptor_set;
		drawables.push_back({*mat_set, entry.obj, mesh});
	}
	std::sort(drawables.begin(), drawables.end(),
		[](const Drawable& a, const Drawable& b) { return a.material_set < b.material_set; });

	VkDescriptorSet bound_material_set = VK_NULL_HANDLE;
	for (const auto& d : drawables) {
		// bind descriptor sets for each material set
		if (d.material_set != bound_material_set) {
			bound_material_set = d.material_set;
			frame_info.command_buffer.bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,
				*m_pipeline_layout,
				0,
				{*frame_info.global_descriptor_set, bound_material_set, *frame_info.shadow_descriptor_set},
				{}
			);
		}

		// render the object
		VeGameObject& obj = *d.obj;
		MeshComponent* mesh = d.mesh;

		// push constants
		SimplePushConstantData push{};
		const glm::mat3 nrm = obj.getNormalTransform();
		push.normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		push.normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		push.normal_transform[2] = glm::vec4(nrm[2], 0.0f);
		push.transform = obj.getTransform();
		push.has_texture = mesh->has_texture;
		frame_info.command_buffer.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);


		mesh->getMesh()->bindVertexBuffer(frame_info.command_buffer);
		mesh->getMesh()->bindIndexBuffer(frame_info.command_buffer);
		mesh->getMesh()->drawIndexed(frame_info.command_buffer);
	}
}

} // namespace ve
