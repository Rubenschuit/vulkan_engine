#include "pch.hpp"
#include "rendering/simple_render_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_mesh.hpp"
#include "utils/ve_log.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <algorithm>
#include <vector>

namespace ve {

// Push constants now only contain per-batch material flags.
// Per-instance transforms are in the SSBO (binding 1, set 0).
struct SimplePushConstantData {
	alignas(4) float has_texture;
	alignas(4) uint32_t instance_offset; // SSBO offset for this batch
	alignas(4) float padding[2];
};
static_assert(sizeof(SimplePushConstantData) == 16, "Push constants must be 16 bytes");

SimpleRenderSystem::SimpleRenderSystem(
	VeDevice& device,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_mask_set_layout,
	const vk::raii::DescriptorSetLayout& cluster_set_layout,
	const vk::raii::DescriptorSetLayout& ao_set_layout,
	vk::Format color_format,
	vk::SampleCountFlagBits sample_count,
	std::filesystem::path shader_path)
	: m_ve_device(device), m_shader_path(shader_path), m_color_format(color_format), m_sample_count(sample_count) {

	createPipelineLayout(global_set_layout, material_set_layout, shadow_set_layout, shadow_mask_set_layout, cluster_set_layout, ao_set_layout);
	createPipelines(m_color_format, m_sample_count);
}

SimpleRenderSystem::~SimpleRenderSystem() {
}

void SimpleRenderSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_mask_set_layout,
	const vk::raii::DescriptorSetLayout& cluster_set_layout,
	const vk::raii::DescriptorSetLayout& ao_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(SimplePushConstantData)
	};
	vk::DescriptorSetLayout layouts[6] = {
		*global_set_layout, *material_set_layout, *shadow_set_layout,
		*shadow_mask_set_layout, *cluster_set_layout, *ao_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 6,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void SimpleRenderSystem::createPipelines(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);
	pipeline_config.multisample_info.rasterizationSamples = sample_count;
	pipeline_config.color_format = color_format;
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eBack;
	pipeline_config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptionsSimple();
	pipeline_config.input_assembly_info.topology = m_topology;
	pipeline_config.pipeline_layout = *m_pipeline_layout;

	// Create pipeline variants: 4 shadow modes × 2 mask states
	for (uint32_t mode = 0; mode < SHADOW_MODE_COUNT; mode++) {
		pipeline_config.specialization_constants = {{0, mode}, {1, m_pcf_samples}, {2, m_pcss_filter_samples}, {3, 0u}};
		m_pipelines[mode] = std::make_unique<VePipeline>(m_ve_device, m_shader_path, pipeline_config);
		pipeline_config.specialization_constants = {{0, mode}, {1, m_pcf_samples}, {2, m_pcss_filter_samples}, {3, 1u}};
		m_pipelines_mask[mode] = std::make_unique<VePipeline>(m_ve_device, m_shader_path, pipeline_config);
	}
}

// Renders visible objects from frame_info.visible_objects. Objects are grouped by
// (mesh, material) for instanced draw calls.
void SimpleRenderSystem::renderObjects(VeFrameInfo& frame_info) const {
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);
	bool mask = frame_info.shadow_mask_active;
	auto& pipeline = mask ? m_pipelines_mask[mode] : m_pipelines[mode];
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());

	auto& registry = *frame_info.registry;
	m_instance_groups.clear();

	// Build sortable drawable list
	struct Drawable {
		VkDescriptorSet material_set;
		Entity entity;
		MeshComponent* mesh = nullptr;
	};
	std::vector<Drawable> drawables;
	drawables.reserve(frame_info.visible_objects.size());
	for (auto& entry : frame_info.visible_objects) {
		MeshComponent* mesh = entry.mesh;
		if (!mesh || !mesh->getMesh() || !registry.getComponent<TransformComponent>(entry.entity))
			continue;
		if (!mesh->getMaterial())
			continue;
		auto* mat = mesh->getMaterial();
		vk::raii::DescriptorSet& mat_set = mat->hasDescriptorSet()
			? mat->getDescriptorSet()
			: frame_info.material_descriptor_set;
		drawables.push_back({*mat_set, entry.entity, mesh});
	}

	// Sort by (mesh pointer, material set) for batching
	std::sort(drawables.begin(), drawables.end(),
		[](const Drawable& a, const Drawable& b) {
			VeMesh* mesh_a = a.mesh->getMesh();
			VeMesh* mesh_b = b.mesh->getMesh();
			if (mesh_a != mesh_b)
				return mesh_a < mesh_b;
			return a.material_set < b.material_set;
		});

	// Write transforms to SSBO and build instance groups
	for (size_t i = 0; i < drawables.size(); ++i) {
		auto& d = drawables[i];
		if (frame_info.instance_count >= frame_info.instance_capacity) {
			VE_LOGW("Instance buffer full (" << frame_info.instance_capacity << " instances), skipping remaining simple objects");
			break;
		}

		uint32_t idx = frame_info.instance_count++;
		const glm::mat3 nrm = registry.getWorldNormal(d.entity);
		frame_info.instance_data[idx].transform = registry.getWorldTransform(d.entity);
		frame_info.instance_data[idx].normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		frame_info.instance_data[idx].normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		frame_info.instance_data[idx].normal_transform[2] = glm::vec4(nrm[2], 0.0f);

		VeMesh* mesh_ptr = d.mesh->getMesh();
		if (!m_instance_groups.empty()) {
			auto& group = m_instance_groups.back();
			if (group.mesh == mesh_ptr && group.material_set == d.material_set) {
				group.instance_count++;
				continue;
			}
		}

		InstanceGroup group{
			.mesh = mesh_ptr,
			.material_set = d.material_set,
			.first_instance = idx,
			.instance_count = 1,
			.has_texture = d.mesh->has_texture
		};
		m_instance_groups.push_back(group);
	}

	// Bind global (set 0), shadow (set 2), and shadow mask (set 3) once
	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*frame_info.global_descriptor_set}, {});
	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		2, {*frame_info.shadow_descriptor_set}, {});
	if (frame_info.shadow_mask_descriptor_set) {
		frame_info.command_buffer.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			3, {**frame_info.shadow_mask_descriptor_set}, {});
	}
	if (frame_info.cluster_descriptor_set) {
		frame_info.command_buffer.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			4, {**frame_info.cluster_descriptor_set}, {});
	}
	if (frame_info.ao_descriptor_set) {
		frame_info.command_buffer.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			5, {**frame_info.ao_descriptor_set}, {});
	}

	// Render instance groups
	VkDescriptorSet bound_material_set = VK_NULL_HANDLE;
	VeMesh* bound_mesh = nullptr;

	for (const auto& group : m_instance_groups) {
		// Bind material descriptor set (set 1) only when it changes
		if (group.material_set != bound_material_set) {
			bound_material_set = group.material_set;
			frame_info.command_buffer.bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,
				*m_pipeline_layout,
				1,
				{bound_material_set},
				{}
			);
		}

		SimplePushConstantData push{
				.has_texture = group.has_texture,
				.instance_offset = group.first_instance
		};
		frame_info.command_buffer.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);

		if (group.mesh != bound_mesh) {
			bound_mesh = group.mesh;
			bound_mesh->bindVertexBuffer(frame_info.command_buffer);
			bound_mesh->bindIndexBuffer(frame_info.command_buffer);
		}

		group.mesh->drawIndexed(frame_info.command_buffer, group.instance_count, 0);
	}
}

} // namespace ve
