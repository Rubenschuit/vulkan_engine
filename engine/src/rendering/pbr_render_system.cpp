#include "pch.hpp"
#include "rendering/pbr_render_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_mesh.hpp"
#include "scene/ve_scene.hpp"
#include "utils/ve_log.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <algorithm>
#include <vector>

namespace ve {

// Push constants contain per-batch material flags + SSBO offset.
// Per-instance transforms are in the SSBO.
struct PbrPushConstantData {
	alignas(4) float has_texture;
	alignas(4) float alpha_cutoff;
	alignas(4) uint32_t material_flags;  // bits 0-1: alpha_mode, bit 2: double_sided, bit 3: flip_tex_coord_v, bit 4: use_spec_gloss_texture
	alignas(4) uint32_t instance_offset; // SSBO offset for this batch
	alignas(4) float depth_offset;       // clip-space Z offset (>0 pushes towards camera)
	alignas(4) uint32_t lod_level;       // LOD level for debug visualization
};
static_assert(sizeof(PbrPushConstantData) == 24, "Push constants must be 24 bytes");

// Clip-space depth offset for MASK (alpha-tested) decal overlays.
// Multiplied by W in the vertex shader, making it distance-proportional.
// Positive values push geometry towards the camera.
static constexpr float MASK_DEPTH_OFFSET = 0.00001f;

PbrRenderSystem::PbrRenderSystem(
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
	: m_ve_device(device), m_shader_path(std::move(shader_path)), m_color_format(color_format), m_sample_count(sample_count) {

	createPipelineLayout(global_set_layout, material_set_layout, shadow_set_layout, shadow_mask_set_layout, cluster_set_layout, ao_set_layout);
	createPipelines(m_color_format, m_sample_count);
}

PbrRenderSystem::~PbrRenderSystem() = default;

void PbrRenderSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_mask_set_layout,
	const vk::raii::DescriptorSetLayout& cluster_set_layout,
	const vk::raii::DescriptorSetLayout& ao_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(PbrPushConstantData)
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

void PbrRenderSystem::createPipelines(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);
	pipeline_config.dynamic_state_enables.push_back(vk::DynamicState::eCullMode);
	pipeline_config.dynamic_state_enables.push_back(vk::DynamicState::eDepthWriteEnable);
	pipeline_config.dynamic_state_enables.push_back(vk::DynamicState::eDepthCompareOp);
	pipeline_config.dynamic_state_enables.push_back(vk::DynamicState::eDepthBias);
	pipeline_config.dynamic_state_info.dynamicStateCount = static_cast<uint32_t>(pipeline_config.dynamic_state_enables.size());
	pipeline_config.dynamic_state_info.pDynamicStates = pipeline_config.dynamic_state_enables.data();
	pipeline_config.rasterization_info.depthBiasEnable = VK_TRUE;
	pipeline_config.multisample_info.rasterizationSamples = sample_count;
	pipeline_config.color_format = color_format;
	pipeline_config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptions();
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

void PbrRenderSystem::prepareFrame(VeFrameInfo& frame_info) const {
	m_opaque_drawables.clear();
	m_transparent_drawables.clear();
	m_opaque_groups.clear();
	const size_t visible_count = frame_info.visible_objects.size();
	m_opaque_drawables.reserve(std::max(visible_count, m_opaque_drawables.capacity()));
	m_transparent_drawables.reserve(std::max(visible_count, m_transparent_drawables.capacity()));

	const glm::vec3 camera_pos = frame_info.camera.getPosition();
	const glm::vec3 camera_fwd = frame_info.camera.getForward();
	auto& registry = *frame_info.registry;

	// 1. Build drawable lists (opaque vs transparent)
	for (auto& entry : frame_info.visible_objects) {
		MeshComponent* mesh = entry.mesh;
		if (!mesh || !registry.getComponent<TransformComponent>(entry.entity))
			continue;
		auto* mat = mesh->getMaterial();
		VeMesh* mesh_ptr = mesh->getMesh();
		vk::raii::DescriptorSet& mat_set = mat->hasDescriptorSet()
			? mat->getDescriptorSet()
			: frame_info.material_descriptor_set;
		MaterialAlphaProps alpha_props = mat->getAlphaProps();
		float transmission = mat->getMaterialFactors().transmission_factor;
		bool use_transparent_pass = (alpha_props.alpha_mode == AlphaMode::BLEND) || (transmission > 0.0f);
		// Use mesh AABB center for sort distance.
		const auto& aabb = mesh->getWorldAABB();
		glm::vec3 obj_pos = (aabb.min + aabb.max) * 0.5f;
		// View-space depth: distance along camera look direction.
		float dist = glm::dot(obj_pos - camera_pos, camera_fwd);
		Drawable d{
			.material_set = *mat_set,
			.entity = entry.entity,
			.mesh = mesh,
			.mesh_ptr = mesh_ptr,
			.material_ptr = mat,
			.dist_sq = dist,
			.alpha_mode = alpha_props.alpha_mode,
			.ssbo_index = 0,
			.lod_level = entry.lod_level
		};
		if (use_transparent_pass)
			m_transparent_drawables.push_back(d);
		else
			m_opaque_drawables.push_back(d);
	}

	// 2. Sort opaques: non-MASK before MASK (rendering order), then by mesh/lod/material (batching).
	//    This ensures base geometry writes depth before alpha-tested overlays, while
	//    keeping mesh/material batching intact within each category.
	std::sort(m_opaque_drawables.begin(), m_opaque_drawables.end(),
		[](const Drawable& a, const Drawable& b) {
			bool a_mask = (a.alpha_mode == AlphaMode::MASK);
			bool b_mask = (b.alpha_mode == AlphaMode::MASK);
			if (a_mask != b_mask)
				return !a_mask; // non-MASK first
			if (a.mesh_ptr != b.mesh_ptr)
				return a.mesh_ptr < b.mesh_ptr;
			if (a.lod_level != b.lod_level)
				return a.lod_level < b.lod_level;
			return a.material_set < b.material_set;
		});

	// 3. Write opaque instance transforms to SSBO and build instance groups
	for (size_t i = 0; i < m_opaque_drawables.size(); ++i) {
		auto& d = m_opaque_drawables[i];
		if (frame_info.instance_count >= frame_info.instance_capacity) {
			VE_LOGW("Instance buffer full (" << frame_info.instance_capacity << " instances), skipping remaining opaque objects");
			m_opaque_drawables.resize(i);
			break;
		}

		// Write transform to SSBO
		uint32_t idx = frame_info.instance_count++;
		d.ssbo_index = idx;
		const glm::mat3 nrm = registry.getWorldNormal(d.entity);
		frame_info.instance_data[idx].transform = registry.getWorldTransform(d.entity);
		frame_info.instance_data[idx].normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		frame_info.instance_data[idx].normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		frame_info.instance_data[idx].normal_transform[2] = glm::vec4(nrm[2], 0.0f);

		// Check if we can merge with the current group
		if (!m_opaque_groups.empty()) {
			auto& group = m_opaque_groups.back();
			if (group.mesh == d.mesh_ptr && group.lod_level == d.lod_level && group.material_set == d.material_set) {
				group.instance_count++;
				continue;
			}
		}

		// Start a new group
		MaterialAlphaProps alpha_props = d.material_ptr ? d.material_ptr->getAlphaProps() : MaterialAlphaProps{};
		InstanceGroup group{
			.mesh = d.mesh_ptr,
			.lod_level = d.lod_level,
			.material_set = d.material_set,
			.first_instance = idx,
			.instance_count = 1,
			.has_texture = d.mesh->has_texture,
			.alpha_cutoff = (alpha_props.alpha_mode == AlphaMode::MASK) ? alpha_props.alpha_cutoff : 0.0f,
			.material_flags = static_cast<uint32_t>(alpha_props.alpha_mode) | (alpha_props.double_sided ? 4u : 0u)
				| (d.material_ptr && d.material_ptr->getFlipTexCoordV() ? 8u : 0u)
				| (alpha_props.use_spec_gloss_texture ? 16u : 0u),
			.double_sided = alpha_props.double_sided
		};
		m_opaque_groups.push_back(group);
	}

	// 4. Sort transparents back-to-front and write their transforms to SSBO
	std::sort(m_transparent_drawables.begin(), m_transparent_drawables.end(),
		[](const Drawable& a, const Drawable& b) { return a.dist_sq > b.dist_sq; });

	for (auto& d : m_transparent_drawables) {
		if (frame_info.instance_count >= frame_info.instance_capacity) {
			VE_LOGW("Instance buffer full (" << frame_info.instance_capacity << " instances), skipping remaining transparent objects");
			break;
		}
		uint32_t idx = frame_info.instance_count++;
		d.ssbo_index = idx;
		const glm::mat3 nrm = registry.getWorldNormal(d.entity);
		frame_info.instance_data[idx].transform = registry.getWorldTransform(d.entity);
		frame_info.instance_data[idx].normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		frame_info.instance_data[idx].normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		frame_info.instance_data[idx].normal_transform[2] = glm::vec4(nrm[2], 0.0f);
	}
}

void PbrRenderSystem::renderOpaqueGroup(
	vk::raii::CommandBuffer& cmd,
	const InstanceGroup& group,
	VkDescriptorSet& bound_material_set,
	VeMesh*& bound_mesh,
	uint32_t& bound_lod) const {

	// Bind material descriptor set (set 1) only when it changes
	if (group.material_set != bound_material_set) {
		bound_material_set = group.material_set;
		cmd.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics,
			*m_pipeline_layout,
			1,
			{bound_material_set},
			{}
		);
	}

	// Set dynamic state
	if (group.double_sided)
		cmd.setCullMode(vk::CullModeFlagBits::eNone);
	else
		cmd.setCullMode(vk::CullModeFlagBits::eBack);

	// Push per-batch material constants
	PbrPushConstantData push{
		.has_texture = group.has_texture,
		.alpha_cutoff = group.alpha_cutoff,
		.material_flags = group.material_flags,
		.instance_offset = group.first_instance,
		.depth_offset = (group.alpha_cutoff > 0.0f) ? MASK_DEPTH_OFFSET : 0.0f,
		.lod_level = group.lod_level
	};
	cmd.pushConstants(
		*m_pipeline_layout,
		vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(PbrPushConstantData), reinterpret_cast<const uint8_t*>(&push))
	);

	// Bind VBO/IBO (if mesh or LOD changed)
	if (group.mesh != bound_mesh || group.lod_level != bound_lod) {
		bound_mesh = group.mesh;
		bound_lod = group.lod_level;
		bound_mesh->bindVertexBuffer(cmd);
		bound_mesh->bindLodIndexBuffer(cmd, bound_lod);
	}

	// Instanced draw (firstInstance=0, shader uses instance_offset push constant for SSBO indexing)
	group.mesh->drawIndexedLod(cmd, group.lod_level, group.instance_count, 0);
}

void PbrRenderSystem::recordOpaqueRange(
	vk::raii::CommandBuffer& cmd, VeFrameInfo& frame_info,
	uint32_t begin_idx, uint32_t end_idx) const {

	if (begin_idx >= end_idx)
		return;

	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);
	bool mask = frame_info.shadow_mask_active;
	auto& pipeline = mask ? m_pipelines_mask[mode] : m_pipelines[mode];
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());

	// Bind global (set 0), shadow (set 2), shadow mask (set 3), cluster (set 4), AO (set 5)
	cmd.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*frame_info.global_descriptor_set}, {});
	cmd.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		2, {*frame_info.shadow_descriptor_set}, {});
	if (frame_info.shadow_mask_descriptor_set) {
		cmd.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			3, {**frame_info.shadow_mask_descriptor_set}, {});
	}
	if (frame_info.cluster_descriptor_set) {
		cmd.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			4, {**frame_info.cluster_descriptor_set}, {});
	}
	if (frame_info.ao_descriptor_set) {
		cmd.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			5, {**frame_info.ao_descriptor_set}, {});
	}

	VkDescriptorSet bound_material_set = VK_NULL_HANDLE;
	VeMesh* bound_mesh = nullptr;
	uint32_t bound_lod = UINT32_MAX;

	// Determine initial depth state based on first group in range
	bool mask_state = (m_opaque_groups[begin_idx].alpha_cutoff > 0.0f);
	cmd.setDepthCompareOp(
		(m_depth_prepass_active || mask_state) ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eLess);
	cmd.setDepthBias(0.0f, 0.0f, 0.0f);
	cmd.setDepthWriteEnable(VK_TRUE);

	for (uint32_t i = begin_idx; i < end_idx; ++i) {
		const auto& group = m_opaque_groups[i];
		if (group.alpha_cutoff > 0.0f && !mask_state) {
			cmd.setDepthCompareOp(vk::CompareOp::eLessOrEqual);
			mask_state = true;
		}
		renderOpaqueGroup(cmd, group, bound_material_set, bound_mesh, bound_lod);
	}
}

void PbrRenderSystem::renderOpaque(VeFrameInfo& frame_info) const {
	recordOpaqueRange(frame_info.cmd(), frame_info,
		0, static_cast<uint32_t>(m_opaque_groups.size()));
}

void PbrRenderSystem::renderTransparent(VeFrameInfo& frame_info) const {
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);
	bool mask = frame_info.shadow_mask_active;
	auto& pipeline = mask ? m_pipelines_mask[mode] : m_pipelines[mode];
	frame_info.cmd().bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());

	// eLessOrEqual so transparent surfaces coplanar with opaque geometry (decal overlays)
	// consistently pass the depth test and render on top.
	frame_info.cmd().setDepthCompareOp(vk::CompareOp::eLessOrEqual);
	frame_info.cmd().setDepthBias(0.0f, 0.0f, 0.0f);

	// Bind global (set 0), shadow (set 2), and shadow mask (set 3) once
	frame_info.cmd().bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*frame_info.global_descriptor_set}, {});
	frame_info.cmd().bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		2, {*frame_info.shadow_descriptor_set}, {});
	if (frame_info.shadow_mask_descriptor_set) {
		frame_info.cmd().bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			3, {**frame_info.shadow_mask_descriptor_set}, {});
	}
	if (frame_info.cluster_descriptor_set) {
		frame_info.cmd().bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			4, {**frame_info.cluster_descriptor_set}, {});
	}
	if (frame_info.ao_descriptor_set) {
		frame_info.cmd().bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			5, {**frame_info.ao_descriptor_set}, {});
	}

	VkDescriptorSet bound_material_set = VK_NULL_HANDLE;
	VeMesh* bound_mesh = nullptr;
	uint32_t bound_lod = UINT32_MAX;

	for (const auto& d : m_transparent_drawables) {
		// Bind material descriptor set (set 1) only when it changes
		if (d.material_set != bound_material_set) {
			bound_material_set = d.material_set;
			frame_info.cmd().bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,
				*m_pipeline_layout,
				1,
				{bound_material_set},
				{}
			);
		}

		MaterialAlphaProps alpha_props = d.material_ptr ? d.material_ptr->getAlphaProps() : MaterialAlphaProps{};

		// Set dynamic state
		if (alpha_props.double_sided)
			frame_info.cmd().setCullMode(vk::CullModeFlagBits::eNone);
		else
			frame_info.cmd().setCullMode(vk::CullModeFlagBits::eBack);
		// No depth writes in the transparent pass. The opaque pass already provides
		// correct occlusion against opaque geometry.
		frame_info.cmd().setDepthWriteEnable(VK_FALSE);
		frame_info.cmd().setDepthBias(0.0f, 0.0f, 0.0f);

		// Push per-object material constants
		PbrPushConstantData push{
			.has_texture = d.mesh->has_texture,
			.alpha_cutoff = alpha_props.alpha_cutoff,
			.material_flags = static_cast<uint32_t>(alpha_props.alpha_mode) | (alpha_props.double_sided ? 4u : 0u)
				| (d.material_ptr && d.material_ptr->getFlipTexCoordV() ? 8u : 0u)
				| (alpha_props.use_spec_gloss_texture ? 16u : 0u),
			.instance_offset = d.ssbo_index,
			.depth_offset = 0.0f,
			.lod_level = d.lod_level
		};
		frame_info.cmd().pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(PbrPushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);

		// Bind VBO/IBO (if mesh or LOD changed)
		if (d.mesh_ptr != bound_mesh || d.lod_level != bound_lod) {
			bound_mesh = d.mesh_ptr;
			bound_lod = d.lod_level;
			bound_mesh->bindVertexBuffer(frame_info.cmd());
			bound_mesh->bindLodIndexBuffer(frame_info.cmd(), bound_lod);
		}

		// Single-instance draw (transparent objects not batched, preserve back-to-front order)
		d.mesh_ptr->drawIndexedLod(frame_info.cmd(), d.lod_level, 1, 0);
	}
}

void PbrRenderSystem::renderObjects(VeFrameInfo& frame_info) const {
	prepareFrame(frame_info);
	renderOpaque(frame_info);
	renderTransparent(frame_info);
}

} // namespace ve
