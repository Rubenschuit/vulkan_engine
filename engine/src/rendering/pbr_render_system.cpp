#include "pch.hpp"
#include "rendering/pbr_render_system.hpp"
#include "rendering/pbr_mega_buffer.hpp"
#include "rendering/material_ssbo_manager.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_mesh.hpp"
#include "scene/ve_scene.hpp"
#include "utils/ve_log.hpp"
#include "utils/ve_frustum.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <algorithm>
#include <vector>

namespace ve {

static constexpr float MASK_DEPTH_OFFSET = 0.00001f;
static constexpr uint32_t INITIAL_INDIRECT_CAPACITY = 4096;

PbrRenderSystem::PbrRenderSystem(
	VeDevice& device,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& bindless_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_mask_set_layout,
	const vk::raii::DescriptorSetLayout& cluster_set_layout,
	const vk::raii::DescriptorSetLayout& ao_set_layout,
	vk::Format color_format,
	vk::SampleCountFlagBits sample_count,
	std::filesystem::path shader_path)
	: m_ve_device(device), m_shader_path(std::move(shader_path)), m_color_format(color_format), m_sample_count(sample_count) {

	m_mega_buffer = std::make_unique<PbrMegaBuffer>(m_ve_device);

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_indirect_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(VkDrawIndexedIndirectCommand), INITIAL_INDIRECT_CAPACITY,
			vk::BufferUsageFlagBits::eIndirectBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_indirect_buffers[i]->map();
	}

	createPipelineLayout(global_set_layout, bindless_set_layout, shadow_set_layout, shadow_mask_set_layout, cluster_set_layout, ao_set_layout);
	createPipelines(m_color_format, m_sample_count);
}

PbrRenderSystem::~PbrRenderSystem() = default;

void PbrRenderSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& bindless_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_mask_set_layout,
	const vk::raii::DescriptorSetLayout& cluster_set_layout,
	const vk::raii::DescriptorSetLayout& ao_set_layout) {

	vk::DescriptorSetLayout layouts[6] = {
		*global_set_layout, *bindless_set_layout, *shadow_set_layout,
		*shadow_mask_set_layout, *cluster_set_layout, *ao_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 6,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 0,
		.pPushConstantRanges = nullptr
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

	for (uint32_t mode = 0; mode < SHADOW_MODE_COUNT; mode++) {
		pipeline_config.specialization_constants = {{0, mode}, {1, m_pcf_samples}, {2, m_pcss_filter_samples}, {3, 0u}};
		m_pipelines[mode] = std::make_unique<VePipeline>(m_ve_device, m_shader_path, pipeline_config);
		pipeline_config.specialization_constants = {{0, mode}, {1, m_pcf_samples}, {2, m_pcss_filter_samples}, {3, 1u}};
		m_pipelines_mask[mode] = std::make_unique<VePipeline>(m_ve_device, m_shader_path, pipeline_config);
	}
}

void PbrRenderSystem::buildMegaBuffer(vk::raii::CommandBuffer& cmd, const std::vector<VeMesh*>& meshes) {
	m_mega_buffer->build(cmd, meshes);
}

void PbrRenderSystem::prepareFrame(VeFrameInfo& frame_info, MaterialSSBOManager& mat_mgr) const {
	m_opaque_drawables.clear();
	m_transparent_drawables.clear();
	const size_t visible_count = frame_info.visible_objects.size();
	m_opaque_drawables.reserve(std::max(visible_count, m_opaque_drawables.capacity()));
	m_transparent_drawables.reserve(std::max(visible_count, m_transparent_drawables.capacity()));

	const glm::vec3 camera_pos = frame_info.camera.getPosition();
	const glm::vec3 camera_fwd = frame_info.camera.getForward();
	auto& registry = *frame_info.registry;

	for (auto& entry : frame_info.visible_objects) {
		MeshComponent* mesh = entry.mesh;
		if (!mesh || !registry.getComponent<TransformComponent>(entry.entity))
			continue;
		auto* mat = mesh->getMaterial();
		VeMesh* mesh_ptr = mesh->getMesh();
		MaterialAlphaProps alpha_props = mat->getAlphaProps();
		float transmission = mat->getMaterialFactors().transmission_factor;
		bool use_transparent_pass = (alpha_props.alpha_mode == AlphaMode::BLEND) || (transmission > 0.0f);
		const auto& aabb = mesh->getWorldAABB();
		glm::vec3 obj_pos = (aabb.min + aabb.max) * 0.5f;
		float dist = glm::dot(obj_pos - camera_pos, camera_fwd);
		Drawable d{
			.entity = entry.entity,
			.mesh = mesh,
			.mesh_ptr = mesh_ptr,
			.material_ptr = mat,
			.dist_sq = dist,
			.alpha_mode = alpha_props.alpha_mode,
			.double_sided = alpha_props.double_sided,
			.ssbo_index = 0,
			.lod_level = entry.lod_level
		};
		if (use_transparent_pass)
			m_transparent_drawables.push_back(d);
		else
			m_opaque_drawables.push_back(d);
	}

	// Sort opaques: (is_mask, double_sided, material, mesh_ptr, lod_level)
	// Grouping by material keeps NonUniformResourceIndex effectively uniform within each
	// draw call, avoiding GPU waterfall/divergence on bindless texture fetches.
	std::sort(m_opaque_drawables.begin(), m_opaque_drawables.end(),
		[](const Drawable& a, const Drawable& b) {
			bool a_mask = (a.alpha_mode == AlphaMode::MASK);
			bool b_mask = (b.alpha_mode == AlphaMode::MASK);
			if (a_mask != b_mask) return !a_mask;
			if (a.double_sided != b.double_sided) return !a.double_sided;
			if (a.material_ptr != b.material_ptr) return a.material_ptr < b.material_ptr;
			if (a.mesh_ptr != b.mesh_ptr) return a.mesh_ptr < b.mesh_ptr;
			return a.lod_level < b.lod_level;
		});

	// Write instance data with material_index
	for (size_t i = 0; i < m_opaque_drawables.size(); ++i) {
		auto& d = m_opaque_drawables[i];
		if (frame_info.instance_count >= frame_info.instance_capacity) {
			VE_LOGW("Instance buffer full (" << frame_info.instance_capacity << ")");
			m_opaque_drawables.resize(i);
			break;
		}

		uint32_t idx = frame_info.instance_count++;
		d.ssbo_index = idx;
		const glm::mat3 nrm = registry.getWorldNormal(d.entity);
		auto& inst = frame_info.instance_data[idx];
		inst.transform = registry.getWorldTransform(d.entity);
		inst.normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		inst.normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		inst.normal_transform[2] = glm::vec4(nrm[2], 0.0f);
		inst.material_index = mat_mgr.registerMaterial(d.material_ptr);
		inst.lod_level = d.lod_level;
		inst.depth_offset = (d.alpha_mode == AlphaMode::MASK) ? MASK_DEPTH_OFFSET : 0.0f;
		MaterialAlphaProps alpha = d.material_ptr ? d.material_ptr->getAlphaProps() : MaterialAlphaProps{};
		inst.material_flags = static_cast<uint32_t>(alpha.alpha_mode)
			| (alpha.double_sided ? 4u : 0u)
			| (d.material_ptr && d.material_ptr->getFlipTexCoordV() ? 8u : 0u)
			| (alpha.use_spec_gloss_texture ? 16u : 0u);
	}

	// Build indirect draw commands directly into persistent vector.
	// Drawables are already sorted by (mask, double_sided, ...) which maps to bucket order,
	// so commands come out in bucket order — no secondary sort needed.
	m_indirect_cmds.clear();
	for (uint32_t b = 0; b < BUCKET_COUNT; b++) {
		m_bucket_offsets[b] = 0;
		m_bucket_counts[b] = 0;
	}

	for (size_t i = 0; i < m_opaque_drawables.size(); ) {
		auto& d = m_opaque_drawables[i];
		const auto* entry = m_mega_buffer->getEntry(d.mesh_ptr);
		if (!entry || d.lod_level >= entry->lod_entries.size()) {
			i++;
			continue;
		}

		bool is_mask = (d.alpha_mode == AlphaMode::MASK);
		bool is_double = d.double_sided;
		const auto& lod = entry->lod_entries[d.lod_level];
		uint32_t first_instance = d.ssbo_index;
		uint32_t instance_count = 1;

		size_t j = i + 1;
		while (j < m_opaque_drawables.size()) {
			auto& d2 = m_opaque_drawables[j];
			if (d2.mesh_ptr != d.mesh_ptr || d2.lod_level != d.lod_level
				|| d2.alpha_mode != d.alpha_mode || d2.double_sided != is_double
				|| d2.material_ptr != d.material_ptr)
				break;
			instance_count++;
			j++;
		}

		uint32_t bucket = (is_mask ? 2u : 0u) + (is_double ? 1u : 0u);
		if (m_bucket_counts[bucket] == 0)
			m_bucket_offsets[bucket] = static_cast<uint32_t>(m_indirect_cmds.size());
		m_bucket_counts[bucket]++;

		m_indirect_cmds.push_back({
			.indexCount = lod.index_count,
			.instanceCount = instance_count,
			.firstIndex = lod.first_index,
			.vertexOffset = static_cast<int32_t>(entry->vertex_offset),
			.firstInstance = first_instance
		});
		i = j;
	}
	m_total_indirect_count = static_cast<uint32_t>(m_indirect_cmds.size());

	// Bulk-write to per-frame indirect buffer (single memcpy)
	auto& indirect_buf = m_indirect_buffers[frame_info.current_frame];
	if (m_indirect_cmds.size() > indirect_buf->getInstanceCount()) {
		uint32_t new_cap = static_cast<uint32_t>(m_indirect_cmds.size() * 2);
		indirect_buf = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(VkDrawIndexedIndirectCommand), new_cap,
			vk::BufferUsageFlagBits::eIndirectBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		indirect_buf->map();
	}
	if (!m_indirect_cmds.empty())
		memcpy(indirect_buf->getMappedMemory(), m_indirect_cmds.data(),
			m_indirect_cmds.size() * sizeof(VkDrawIndexedIndirectCommand));

	// Transparents: back-to-front sort + write SSBO
	std::sort(m_transparent_drawables.begin(), m_transparent_drawables.end(),
		[](const Drawable& a, const Drawable& b) { return a.dist_sq > b.dist_sq; });

	for (auto& d : m_transparent_drawables) {
		if (frame_info.instance_count >= frame_info.instance_capacity) {
			VE_LOGW("Instance buffer full, skipping remaining transparent objects");
			break;
		}
		uint32_t idx = frame_info.instance_count++;
		d.ssbo_index = idx;
		const glm::mat3 nrm = registry.getWorldNormal(d.entity);
		auto& inst = frame_info.instance_data[idx];
		inst.transform = registry.getWorldTransform(d.entity);
		inst.normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		inst.normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		inst.normal_transform[2] = glm::vec4(nrm[2], 0.0f);
		inst.material_index = mat_mgr.registerMaterial(d.material_ptr);
		inst.lod_level = d.lod_level;
		inst.depth_offset = 0.0f;
		MaterialAlphaProps alpha = d.material_ptr ? d.material_ptr->getAlphaProps() : MaterialAlphaProps{};
		inst.material_flags = static_cast<uint32_t>(alpha.alpha_mode)
			| (alpha.double_sided ? 4u : 0u)
			| (d.material_ptr && d.material_ptr->getFlipTexCoordV() ? 8u : 0u)
			| (alpha.use_spec_gloss_texture ? 16u : 0u);
	}
}

void PbrRenderSystem::prepareTransparents(VeFrameInfo& frame_info, MaterialSSBOManager& mat_mgr,
                                           const std::vector<uint32_t>& transparent_entity_indices) const {
	m_transparent_drawables.clear();
	if (transparent_entity_indices.empty())
		return;

	auto& registry = *frame_info.registry;
	FrustumPlane planes[6];
	extractFrustumPlanes(frame_info.camera.getProj() * frame_info.camera.getView(), planes);

	const glm::vec3 camera_pos = frame_info.camera.getPosition();
	const glm::vec3 camera_fwd = frame_info.camera.getForward();

	for (uint32_t entity_idx : transparent_entity_indices) {
		MeshComponent* mesh = registry.meshes().get(entity_idx);
		if (!mesh || !mesh->hasMesh() || !mesh->hasMaterial())
			continue;
		const auto& aabb = mesh->getWorldAABB();
		if (!isAABBInFrustum(aabb, planes))
			continue;

		auto* mat = mesh->getMaterial();
		MaterialAlphaProps alpha_props = mat->getAlphaProps();
		Entity entity = registry.entityFromIndex(entity_idx);
		glm::vec3 obj_pos = (aabb.min + aabb.max) * 0.5f;
		float dist = glm::dot(obj_pos - camera_pos, camera_fwd);
		m_transparent_drawables.push_back({
			.entity = entity,
			.mesh = mesh,
			.mesh_ptr = mesh->getMesh(),
			.material_ptr = mat,
			.dist_sq = dist,
			.alpha_mode = alpha_props.alpha_mode,
			.double_sided = alpha_props.double_sided,
			.ssbo_index = 0,
			.lod_level = mesh->cached_lod
		});
	}

	std::sort(m_transparent_drawables.begin(), m_transparent_drawables.end(),
		[](const Drawable& a, const Drawable& b) { return a.dist_sq > b.dist_sq; });

	for (auto& d : m_transparent_drawables) {
		if (frame_info.instance_count >= frame_info.instance_capacity) {
			VE_LOGW("Instance buffer full, skipping remaining transparent objects");
			break;
		}
		uint32_t idx = frame_info.instance_count++;
		d.ssbo_index = idx;
		const glm::mat3 nrm = registry.getWorldNormal(d.entity);
		auto& inst = frame_info.instance_data[idx];
		inst.transform = registry.getWorldTransform(d.entity);
		inst.normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		inst.normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		inst.normal_transform[2] = glm::vec4(nrm[2], 0.0f);
		inst.material_index = mat_mgr.registerMaterial(d.material_ptr);
		inst.lod_level = d.lod_level;
		inst.depth_offset = 0.0f;
		MaterialAlphaProps alpha = d.material_ptr ? d.material_ptr->getAlphaProps() : MaterialAlphaProps{};
		inst.material_flags = static_cast<uint32_t>(alpha.alpha_mode)
			| (alpha.double_sided ? 4u : 0u)
			| (d.material_ptr && d.material_ptr->getFlipTexCoordV() ? 8u : 0u)
			| (alpha.use_spec_gloss_texture ? 16u : 0u);
	}
}

void PbrRenderSystem::renderOpaqueGpuCulled(
	VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set,
	const VeBuffer& indirect_buffer,
	const uint32_t* bucket_group_offsets,
	const uint32_t* bucket_group_counts,
	const VeBuffer* compacted_buffer,
	const VeBuffer* compact_count_buffer,
	const vk::raii::DescriptorSet* global_set_override) const {

	if (!m_mega_buffer->isValid())
		return;

	auto& cmd = frame_info.cmd();
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);
	bool mask = frame_info.shadow_mask_active;
	auto& pipeline = mask ? m_pipelines_mask[mode] : m_pipelines[mode];
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());

	auto& global_set = global_set_override ? *global_set_override : frame_info.global_descriptor_set;
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*global_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		1, {*bindless_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		2, {*frame_info.shadow_descriptor_set}, {});
	if (frame_info.shadow_mask_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			3, {**frame_info.shadow_mask_descriptor_set}, {});
	if (frame_info.cluster_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			4, {**frame_info.cluster_descriptor_set}, {});
	if (frame_info.ao_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			5, {**frame_info.ao_descriptor_set}, {});

	m_mega_buffer->bind(cmd);
	cmd.setDepthBias(0.0f, 0.0f, 0.0f);
	cmd.setDepthWriteEnable(VK_TRUE);

	for (uint32_t bucket = 0; bucket < BUCKET_COUNT; bucket++) {
		if (bucket_group_counts[bucket] == 0)
			continue;
		bool is_mask = (bucket >= 2);
		bool is_double_sided = (bucket & 1);
		cmd.setCullMode(is_double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);
		cmd.setDepthCompareOp(
			(m_depth_prepass_active || is_mask) ? vk::CompareOp::eGreaterOrEqual : vk::CompareOp::eGreater);
		auto offset = static_cast<vk::DeviceSize>(bucket_group_offsets[bucket]) * sizeof(VkDrawIndexedIndirectCommand);
		if (compacted_buffer && compact_count_buffer) {
			cmd.drawIndexedIndirectCount(
				*compacted_buffer->getBuffer(), offset,
				*compact_count_buffer->getBuffer(), bucket * sizeof(uint32_t),
				bucket_group_counts[bucket], sizeof(VkDrawIndexedIndirectCommand));
		} else {
			cmd.drawIndexedIndirect(
				*indirect_buffer.getBuffer(), offset,
				bucket_group_counts[bucket], sizeof(VkDrawIndexedIndirectCommand));
		}
	}
}

void PbrRenderSystem::renderOpaqueGpuCulledMeshlets(
	VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set,
	const VeBuffer& meshlet_indirect, const VeBuffer& draw_counts,
	const uint32_t* cpu_draw_counts,
	const vk::raii::DescriptorSet* global_set_override) const {

	if (!m_mega_buffer->hasMeshletData())
		return;

	auto& cmd = frame_info.cmd();
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);
	bool mask = frame_info.shadow_mask_active;
	auto& pipeline = mask ? m_pipelines_mask[mode] : m_pipelines[mode];
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());

	auto& global_set = global_set_override ? *global_set_override : frame_info.global_descriptor_set;
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*global_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		1, {*bindless_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		2, {*frame_info.shadow_descriptor_set}, {});
	if (frame_info.shadow_mask_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			3, {**frame_info.shadow_mask_descriptor_set}, {});
	if (frame_info.cluster_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			4, {**frame_info.cluster_descriptor_set}, {});
	if (frame_info.ao_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			5, {**frame_info.ao_descriptor_set}, {});

	m_mega_buffer->bindMeshletIbo(cmd);
	cmd.setDepthBias(0.0f, 0.0f, 0.0f);
	cmd.setDepthWriteEnable(VK_TRUE);

	constexpr uint32_t MAX_PER_BUCKET = MAX_MESHLET_DRAWS / MESHLET_BUCKET_COUNT;

	constexpr uint32_t OPAQUE_MASK_BUCKETS = 4; // buckets 0..3 only; transparent buckets 4-5 handled by WBOIT
	for (uint32_t bucket = 0; bucket < OPAQUE_MASK_BUCKETS; bucket++) {
		bool is_mask_bucket  = (bucket >= 2);
		bool is_double_sided = (bucket & 1) != 0;
		cmd.setCullMode(is_double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);
		cmd.setDepthCompareOp(
			(m_depth_prepass_active || is_mask_bucket)
				? vk::CompareOp::eGreaterOrEqual : vk::CompareOp::eGreater);

		auto buf_offset   = static_cast<vk::DeviceSize>(bucket) * MAX_PER_BUCKET
		                    * sizeof(VkDrawIndexedIndirectCommand);
		if (cpu_draw_counts) {
			uint32_t count = std::min(cpu_draw_counts[bucket] * 2 + 1024, MAX_PER_BUCKET);
			cmd.drawIndexedIndirect(
				*meshlet_indirect.getBuffer(), buf_offset,
				count, sizeof(VkDrawIndexedIndirectCommand));
		} else {
			auto count_offset = static_cast<vk::DeviceSize>(bucket) * sizeof(uint32_t);
			cmd.drawIndexedIndirectCount(
				*meshlet_indirect.getBuffer(), buf_offset,
				*draw_counts.getBuffer(), count_offset,
				MAX_PER_BUCKET, sizeof(VkDrawIndexedIndirectCommand));
		}
	}
}

void PbrRenderSystem::renderOpaque(VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set) const {
	if (m_total_indirect_count == 0 || !m_mega_buffer->isValid())
		return;

	auto& cmd = frame_info.cmd();
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);
	bool mask = frame_info.shadow_mask_active;
	auto& pipeline = mask ? m_pipelines_mask[mode] : m_pipelines[mode];
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());

	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*frame_info.global_descriptor_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		1, {*bindless_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		2, {*frame_info.shadow_descriptor_set}, {});
	if (frame_info.shadow_mask_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			3, {**frame_info.shadow_mask_descriptor_set}, {});
	if (frame_info.cluster_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			4, {**frame_info.cluster_descriptor_set}, {});
	if (frame_info.ao_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			5, {**frame_info.ao_descriptor_set}, {});

	m_mega_buffer->bind(cmd);
	cmd.setDepthBias(0.0f, 0.0f, 0.0f);
	cmd.setDepthWriteEnable(VK_TRUE);

	for (uint32_t bucket = 0; bucket < BUCKET_COUNT; bucket++) {
		if (m_bucket_counts[bucket] == 0) continue;
		bool is_mask = (bucket >= 2);
		bool is_double_sided = (bucket & 1);
		cmd.setCullMode(is_double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);
		cmd.setDepthCompareOp(
			(m_depth_prepass_active || is_mask) ? vk::CompareOp::eGreaterOrEqual : vk::CompareOp::eGreater);
		cmd.drawIndexedIndirect(
			*m_indirect_buffers[frame_info.current_frame]->getBuffer(),
			m_bucket_offsets[bucket] * sizeof(VkDrawIndexedIndirectCommand),
			m_bucket_counts[bucket],
			sizeof(VkDrawIndexedIndirectCommand));
	}
}

void PbrRenderSystem::renderTransparent(VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set,
                                         const vk::raii::DescriptorSet* global_set_override) const {
	if (m_transparent_drawables.empty() || !m_mega_buffer->isValid())
		return;

	auto& cmd = frame_info.cmd();
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);
	bool mask = frame_info.shadow_mask_active;
	auto& pipeline = mask ? m_pipelines_mask[mode] : m_pipelines[mode];
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());

	cmd.setDepthCompareOp(vk::CompareOp::eGreaterOrEqual);
	cmd.setDepthBias(0.0f, 0.0f, 0.0f);

	const auto& global_set = global_set_override ? *global_set_override : frame_info.global_descriptor_set;
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		0, {*global_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		1, {*bindless_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
		2, {*frame_info.shadow_descriptor_set}, {});
	if (frame_info.shadow_mask_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			3, {**frame_info.shadow_mask_descriptor_set}, {});
	if (frame_info.cluster_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			4, {**frame_info.cluster_descriptor_set}, {});
	if (frame_info.ao_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			5, {**frame_info.ao_descriptor_set}, {});

	m_mega_buffer->bind(cmd);

	for (const auto& d : m_transparent_drawables) {
		const auto* entry = m_mega_buffer->getEntry(d.mesh_ptr);
		if (!entry || d.lod_level >= entry->lod_entries.size())
			continue;
		MaterialAlphaProps alpha_props = d.material_ptr ? d.material_ptr->getAlphaProps() : MaterialAlphaProps{};
		cmd.setCullMode(alpha_props.double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);
		cmd.setDepthWriteEnable(VK_FALSE);
		cmd.setDepthBias(0.0f, 0.0f, 0.0f);
		const auto& lod = entry->lod_entries[d.lod_level];
		cmd.drawIndexed(lod.index_count, 1, lod.first_index,
			static_cast<int32_t>(entry->vertex_offset), d.ssbo_index);
	}
}

void PbrRenderSystem::bindPbrResources(VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set,
                                        const vk::raii::DescriptorSet* global_set_override) const {
	auto& cmd = frame_info.cmd();
	auto& global_set = global_set_override ? *global_set_override : frame_info.global_descriptor_set;
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout, 0, {*global_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout, 1, {*bindless_set}, {});
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout, 2, {*frame_info.shadow_descriptor_set}, {});
	if (frame_info.shadow_mask_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout, 3, {**frame_info.shadow_mask_descriptor_set}, {});
	if (frame_info.cluster_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout, 4, {**frame_info.cluster_descriptor_set}, {});
	if (frame_info.ao_descriptor_set)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout, 5, {**frame_info.ao_descriptor_set}, {});
	m_mega_buffer->bind(cmd);
}

void PbrRenderSystem::createWboitGeometryPipelines() {
	auto wboit_shader_path = m_shader_path.parent_path() / "pbr_wboit.spv";

	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.dynamic_state_enables.push_back(vk::DynamicState::eCullMode);
	config.dynamic_state_enables.push_back(vk::DynamicState::eDepthWriteEnable);
	config.dynamic_state_enables.push_back(vk::DynamicState::eDepthCompareOp);
	config.dynamic_state_enables.push_back(vk::DynamicState::eDepthBias);
	config.dynamic_state_info.dynamicStateCount = static_cast<uint32_t>(config.dynamic_state_enables.size());
	config.dynamic_state_info.pDynamicStates = config.dynamic_state_enables.data();
	config.rasterization_info.depthBiasEnable = VK_TRUE;
	config.multisample_info.rasterizationSamples = vk::SampleCountFlagBits::e1;
	config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptions();
	config.input_assembly_info.topology = m_topology;
	config.pipeline_layout = *m_pipeline_layout;

	// MRT: 2 color attachments (accum RGBA16F, revealage R16F)
	config.color_attachment_formats = {vk::Format::eR16G16B16A16Sfloat, vk::Format::eR16Sfloat};

	// Accum blend
	vk::PipelineColorBlendAttachmentState accum_blend{
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = vk::BlendFactor::eOne,
		.dstColorBlendFactor = vk::BlendFactor::eOne,
		.colorBlendOp = vk::BlendOp::eAdd,
		.srcAlphaBlendFactor = vk::BlendFactor::eOne,
		.dstAlphaBlendFactor = vk::BlendFactor::eOne,
		.alphaBlendOp = vk::BlendOp::eAdd,
		.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
		                | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
	};
	// Revealage blend
	vk::PipelineColorBlendAttachmentState revealage_blend{
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = vk::BlendFactor::eZero,
		.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor,
		.colorBlendOp = vk::BlendOp::eAdd,
		.srcAlphaBlendFactor = vk::BlendFactor::eZero,
		.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
		.alphaBlendOp = vk::BlendOp::eAdd,
		.colorWriteMask = vk::ColorComponentFlagBits::eR
	};
	config.color_blend_attachments = {accum_blend, revealage_blend};

	// Depth test on, write off (set dynamically)
	config.depth_stencil_info.depthTestEnable = VK_TRUE;
	config.depth_stencil_info.depthWriteEnable = VK_FALSE;

	for (uint32_t mode = 0; mode < SHADOW_MODE_COUNT; mode++) {
		config.specialization_constants = {{0, mode}, {1, m_pcf_samples}, {2, m_pcss_filter_samples}, {3, 0u}};
		m_wboit_pipelines[mode] = std::make_unique<VePipeline>(m_ve_device, wboit_shader_path, config);
	}
}

void PbrRenderSystem::initWboit(const vk::raii::ImageView& accum_view, const vk::raii::ImageView& revealage_view,
                                 vk::Format resolve_format) {
	// Create WBOIT geometry pipelines
	createWboitGeometryPipelines();

	// Create composite descriptor set layout (2 combined image samplers)
	m_wboit_composite_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();

	m_wboit_composite_pool = VeDescriptorPool::Builder(m_ve_device)
		.setMaxSets(1)
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, 2)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
		.build();

	// Create sampler
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eLinear,
		.addressModeU = vk::SamplerAddressMode::eClampToEdge,
		.addressModeV = vk::SamplerAddressMode::eClampToEdge,
		.addressModeW = vk::SamplerAddressMode::eClampToEdge,
		.mipLodBias = 0.0f,
		.anisotropyEnable = vk::False,
		.maxAnisotropy = 1.0f,
		.compareEnable = vk::False,
		.compareOp = vk::CompareOp::eAlways,
		.minLod = 0.0f,
		.maxLod = 1.0f,
		.borderColor = vk::BorderColor::eIntOpaqueBlack,
		.unnormalizedCoordinates = vk::False
	};
	m_wboit_composite_sampler = std::make_unique<vk::raii::Sampler>(m_ve_device.getDevice(), sampler_info);

	// Write descriptor set
	vk::DescriptorImageInfo accum_info{
		.sampler = **m_wboit_composite_sampler,
		.imageView = *accum_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};
	vk::DescriptorImageInfo revealage_info{
		.sampler = **m_wboit_composite_sampler,
		.imageView = *revealage_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};
	VeDescriptorWriter(*m_wboit_composite_set_layout, *m_wboit_composite_pool)
		.writeImage(0, &accum_info)
		.writeImage(1, &revealage_info)
		.build(m_wboit_composite_set);

	// Composite pipeline layout
	vk::DescriptorSetLayout layouts[1] = {m_wboit_composite_set_layout->getDescriptorSetLayout()};
	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = 1,
		.pSetLayouts = layouts,
	};
	m_wboit_composite_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);

	// Composite pipeline
	auto composite_shader_path = m_shader_path.parent_path() / "wboit_composite.spv";
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.multisample_info.rasterizationSamples = vk::SampleCountFlagBits::e1;
	config.color_format = resolve_format;
	config.pipeline_layout = *m_wboit_composite_pipeline_layout;
	config.attribute_descriptions.clear();
	config.binding_descriptions.clear();
	config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	config.depth_stencil_info.depthTestEnable = vk::False;
	config.depth_stencil_info.depthWriteEnable = vk::False;

	// Alpha blending: SrcAlpha / OneMinusSrcAlpha (composite WBOIT over opaque scene)
	config.color_blend_attachment.blendEnable = VK_TRUE;
	config.color_blend_attachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
	config.color_blend_attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
	config.color_blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
	config.color_blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
	config.color_blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;

	m_wboit_composite_pipeline = std::make_unique<VePipeline>(m_ve_device, composite_shader_path, config);
}

void PbrRenderSystem::renderTransparentWboit(
	VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set,
	const VeBuffer& indirect_buffer,
	const uint32_t* bucket_group_offsets,
	const uint32_t* bucket_group_counts,
	const VeBuffer* compacted_buffer,
	const VeBuffer* compact_count_buffer,
	const vk::raii::DescriptorSet* global_set_override) const {

	if (!m_mega_buffer->isValid() || !m_wboit_pipelines[0])
		return;

	auto& cmd = frame_info.cmd();
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_wboit_pipelines[mode]->getPipeline());

	bindPbrResources(frame_info, bindless_set, global_set_override);
	cmd.setDepthBias(0.0f, 0.0f, 0.0f);
	cmd.setDepthWriteEnable(VK_FALSE);
	cmd.setDepthCompareOp(vk::CompareOp::eGreaterOrEqual);

	// Buckets 4-5: transparent back-face / transparent double-sided
	for (uint32_t bucket = 4; bucket < GPU_CULL_BUCKET_COUNT; bucket++) {
		if (bucket_group_counts[bucket] == 0)
			continue;
		bool is_double_sided = (bucket & 1);
		cmd.setCullMode(is_double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);
		auto offset = static_cast<vk::DeviceSize>(bucket_group_offsets[bucket]) * sizeof(VkDrawIndexedIndirectCommand);
		if (compacted_buffer && compact_count_buffer) {
			cmd.drawIndexedIndirectCount(
				*compacted_buffer->getBuffer(), offset,
				*compact_count_buffer->getBuffer(), bucket * sizeof(uint32_t),
				bucket_group_counts[bucket], sizeof(VkDrawIndexedIndirectCommand));
		} else {
			cmd.drawIndexedIndirect(
				*indirect_buffer.getBuffer(), offset,
				bucket_group_counts[bucket], sizeof(VkDrawIndexedIndirectCommand));
		}
	}
}

void PbrRenderSystem::renderTransparentWboitMeshlets(
	VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set,
	const VeBuffer& meshlet_indirect, const VeBuffer& draw_counts,
	const uint32_t* cpu_draw_counts,
	const vk::raii::DescriptorSet* global_set_override) const {

	if (!m_mega_buffer->hasMeshletData() || !m_wboit_pipelines[0])
		return;

	auto& cmd = frame_info.cmd();
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_wboit_pipelines[mode]->getPipeline());

	bindPbrResources(frame_info, bindless_set, global_set_override);
	m_mega_buffer->bindMeshletIbo(cmd);
	cmd.setDepthBias(0.0f, 0.0f, 0.0f);
	cmd.setDepthWriteEnable(VK_FALSE);
	cmd.setDepthCompareOp(vk::CompareOp::eGreaterOrEqual);

	constexpr uint32_t MAX_PER_BUCKET = MAX_MESHLET_DRAWS / MESHLET_BUCKET_COUNT;

	// Buckets 4-5: transparent back-face / transparent double-sided
	for (uint32_t bucket = 4; bucket < MESHLET_BUCKET_COUNT; bucket++) {
		bool is_double_sided = (bucket & 1) != 0;
		cmd.setCullMode(is_double_sided ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);

		auto buf_offset = static_cast<vk::DeviceSize>(bucket) * MAX_PER_BUCKET
		                  * sizeof(VkDrawIndexedIndirectCommand);
		if (cpu_draw_counts) {
			uint32_t count = std::min(cpu_draw_counts[bucket] * 2 + 1024, MAX_PER_BUCKET);
			if (count == 0) continue;
			cmd.drawIndexedIndirect(
				*meshlet_indirect.getBuffer(), buf_offset,
				count, sizeof(VkDrawIndexedIndirectCommand));
		} else {
			auto count_offset = static_cast<vk::DeviceSize>(bucket) * sizeof(uint32_t);
			cmd.drawIndexedIndirectCount(
				*meshlet_indirect.getBuffer(), buf_offset,
				*draw_counts.getBuffer(), count_offset,
				MAX_PER_BUCKET, sizeof(VkDrawIndexedIndirectCommand));
		}
	}
}

void PbrRenderSystem::compositeWboit(vk::raii::CommandBuffer& command_buffer) const {
	if (!m_wboit_composite_pipeline)
		return;
	command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_wboit_composite_pipeline->getPipeline());
	command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
		*m_wboit_composite_pipeline_layout, 0, {*m_wboit_composite_set}, {});
	command_buffer.draw(3, 1, 0, 0);
}

void PbrRenderSystem::recreateWboit(const vk::raii::ImageView& accum_view, const vk::raii::ImageView& revealage_view,
                                     vk::Format resolve_format) {
	// Recreate geometry pipelines (shadow samples may have changed)
	for (auto& p : m_wboit_pipelines)
		p.reset();
	createWboitGeometryPipelines();

	// Recreate composite descriptor set with new image views
	m_wboit_composite_set = nullptr;
	if (m_wboit_composite_pool)
		m_wboit_composite_pool->resetPool();

	vk::DescriptorImageInfo accum_info{
		.sampler = **m_wboit_composite_sampler,
		.imageView = *accum_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};
	vk::DescriptorImageInfo revealage_info{
		.sampler = **m_wboit_composite_sampler,
		.imageView = *revealage_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};
	VeDescriptorWriter(*m_wboit_composite_set_layout, *m_wboit_composite_pool)
		.writeImage(0, &accum_info)
		.writeImage(1, &revealage_info)
		.build(m_wboit_composite_set);

	// Recreate composite pipeline
	m_wboit_composite_pipeline.reset();
	auto composite_shader_path = m_shader_path.parent_path() / "wboit_composite.spv";
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.multisample_info.rasterizationSamples = vk::SampleCountFlagBits::e1;
	config.color_format = resolve_format;
	config.pipeline_layout = *m_wboit_composite_pipeline_layout;
	config.attribute_descriptions.clear();
	config.binding_descriptions.clear();
	config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	config.depth_stencil_info.depthTestEnable = vk::False;
	config.depth_stencil_info.depthWriteEnable = vk::False;
	config.color_blend_attachment.blendEnable = VK_TRUE;
	config.color_blend_attachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
	config.color_blend_attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
	config.color_blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
	config.color_blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
	config.color_blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;
	m_wboit_composite_pipeline = std::make_unique<VePipeline>(m_ve_device, composite_shader_path, config);
}

} // namespace ve
