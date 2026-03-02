#include "pch.hpp"
#include "rendering/gpu_scene_manager.hpp"
#include "rendering/pbr_mega_buffer.hpp"
#include "rendering/material_ssbo_manager.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"
#include "utils/ve_log.hpp"

#include <algorithm>
#include <tuple>

namespace ve {

GpuSceneManager::GpuSceneManager(VeDevice& device) : m_ve_device(device) {
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		// Device-local for fast GPU compute reads
		m_object_data_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(ObjectDataGPU), MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_transform_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(TransformGPU), MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_active_id_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(ActiveIdEntry), MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_draw_group_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(DrawGroupGPU), MAX_DRAW_GROUPS,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		// Host-visible staging for CPU to GPU copies
		m_object_data_staging[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(ObjectDataGPU), MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_object_data_staging[i]->map();

		m_transform_staging[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(TransformGPU), MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_transform_staging[i]->map();

		m_active_id_staging[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(ActiveIdEntry), MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_active_id_staging[i]->map();

		m_draw_group_staging[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(DrawGroupGPU), MAX_DRAW_GROUPS,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_draw_group_staging[i]->map();

		m_indirect_staging[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(VkDrawIndexedIndirectCommand), MAX_DRAW_GROUPS,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_indirect_staging[i]->map();

		m_indirect_template[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(VkDrawIndexedIndirectCommand), MAX_DRAW_GROUPS,
			vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);
	}

	m_dirty_frame.resize(MAX_GPU_OBJECTS, 0);
	m_object_lod_group_ids.resize(MAX_GPU_OBJECTS);
	m_cpu_lod_data.resize(MAX_GPU_OBJECTS);
	m_is_transparent_by_gpu_id.resize(MAX_GPU_OBJECTS, false);
}

GpuSceneManager::~GpuSceneManager() = default;

void GpuSceneManager::subscribeToRegistry(Registry& registry) {
	unsubscribeFromRegistry();
	m_registry = &registry;

	m_mesh_removed_sub = registry.events().subscribe<ComponentRemovedEvent<MeshComponent>>(
		[this](const ComponentRemovedEvent<MeshComponent>& event) {
			unregisterObject(event.entity);
		});
	m_transform_invalidated_sub = registry.events().subscribe<TransformInvalidatedEvent>(
		[this](const TransformInvalidatedEvent& event) {
			markTransformDirty(event.entity);
		});
	m_mesh_data_changed_sub = registry.events().subscribe<MeshDataChangedEvent>(
		[this](const MeshDataChangedEvent& event) {
			markObjectDataDirty(event.entity);
		});
	m_mesh_added_sub = registry.events().subscribe<ComponentAddedEvent<MeshComponent>>(
		[this](const ComponentAddedEvent<MeshComponent>& event) {
			if (!m_mega_buffer || !m_mat_mgr || !m_registry)
				return;
			if (!event.component.hasMesh())
				return;
			if (!m_mega_buffer->getEntry(event.component.getMesh()))
				return;
			registerObject(event.entity, event.component, *m_mega_buffer, *m_mat_mgr, *m_registry);
		});
}

uint32_t GpuSceneManager::registerObject(Entity entity, const MeshComponent& mesh,
                                          const PbrMegaBuffer& mega_buffer,
                                          MaterialSSBOManager& mat_mgr,
                                          const Registry& registry) {
	auto it = m_entity_to_gpu_id.find(entity.index());
	if (it != m_entity_to_gpu_id.end())
		return it->second;

	uint32_t gpu_id = allocateGpuId();
	m_entity_to_gpu_id[entity.index()] = gpu_id;
	m_gpu_id_to_entity[gpu_id] = entity;
	m_active_count++;

	writeObjectData(gpu_id, mesh, mega_buffer, mat_mgr);

	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++)
		writeTransform(gpu_id, f, registry, entity);

	m_dirty_frame[gpu_id] = m_global_frame_counter;
	m_object_data_dirty.fill(true);
	m_template_needs_copy.fill(true);
	m_draw_groups_dirty = true;

	return gpu_id;
}

void GpuSceneManager::unregisterObject(Entity entity) {
	auto it = m_entity_to_gpu_id.find(entity.index());
	if (it == m_entity_to_gpu_id.end())
		return;

	uint32_t gpu_id = it->second;

	if (m_is_transparent_by_gpu_id[gpu_id]) {
		uint32_t entity_idx = entity.index();
		auto jt = std::find(m_transparent_entity_indices.begin(),
		                    m_transparent_entity_indices.end(), entity_idx);
		if (jt != m_transparent_entity_indices.end()) {
			*jt = m_transparent_entity_indices.back();
			m_transparent_entity_indices.pop_back();
		}
		m_is_transparent_by_gpu_id[gpu_id] = false;
	}

	freeGpuId(gpu_id);
	m_gpu_id_to_entity.erase(gpu_id);
	m_entity_to_gpu_id.erase(it);
	m_active_count--;

	// Zero out the slot in staging so GPU culling skips it
	ObjectDataGPU zero{};
	vk::DeviceSize offset = static_cast<vk::DeviceSize>(gpu_id) * sizeof(ObjectDataGPU);
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++)
		m_object_data_staging[f]->writeToBuffer(&zero, sizeof(ObjectDataGPU), offset);
	m_object_data_dirty.fill(true);
	m_template_needs_copy.fill(true);
	m_draw_groups_dirty = true;
}

void GpuSceneManager::markTransformDirty(Entity entity) {
	auto it = m_entity_to_gpu_id.find(entity.index());
	if (it == m_entity_to_gpu_id.end())
		return;
	m_dirty_frame[it->second] = m_global_frame_counter;
}

void GpuSceneManager::markObjectDataDirty(Entity entity) {
	auto it = m_entity_to_gpu_id.find(entity.index());
	if (it == m_entity_to_gpu_id.end() || !m_mega_buffer || !m_mat_mgr || !m_registry)
		return;
	auto* mesh = m_registry->getComponent<MeshComponent>(entity);
	if (!mesh)
		return;
	writeObjectData(it->second, *mesh, *m_mega_buffer, *m_mat_mgr);
	m_object_data_dirty.fill(true);
	m_template_needs_copy.fill(true);
	m_draw_groups_dirty = true;
}

void GpuSceneManager::updateDirtyTransforms(uint32_t current_frame, const Registry& registry,
                                             vk::raii::CommandBuffer& cmd) {
	m_global_frame_counter++;
	uint32_t last_written = m_buffer_last_written[current_frame];

	std::vector<vk::BufferCopy> copy_regions;
	copy_regions.reserve(64);

	for (auto& [entity_idx, gpu_id] : m_entity_to_gpu_id) {
		if (m_dirty_frame[gpu_id] < last_written)
			continue;
		Entity entity = m_gpu_id_to_entity[gpu_id];
		if (!registry.isAlive(entity))
			continue;

		writeTransform(gpu_id, current_frame, registry, entity);

		vk::DeviceSize offset = static_cast<vk::DeviceSize>(gpu_id) * sizeof(TransformGPU);
		copy_regions.push_back({offset, offset, sizeof(TransformGPU)});
	}
	if (!copy_regions.empty()) {
		cmd.copyBuffer(*m_transform_staging[current_frame]->getBuffer(),
		               *m_transform_buffers[current_frame]->getBuffer(),
		               copy_regions);
	}

	// Copy object data + rebuild draw groups if structure changed (add/remove)
	bool did_object_copy = false;
	if (m_object_data_dirty[current_frame]) {
		vk::DeviceSize size = static_cast<vk::DeviceSize>(m_next_id) * sizeof(ObjectDataGPU);
		if (size > 0) {
			vk::BufferCopy full_copy{0, 0, size};
			cmd.copyBuffer(*m_object_data_staging[current_frame]->getBuffer(),
			               *m_object_data_buffers[current_frame]->getBuffer(),
			               full_copy);
			did_object_copy = true;
		}

		// Rebuild draw groups and active IDs
		if (m_draw_groups_dirty) {
			rebuildDrawGroups();
			m_draw_groups_dirty = false;
		}

		// Upload active IDs (gpu_id + draw_group_id pairs)
		if (!m_active_ids.empty()) {
			vk::DeviceSize id_size = static_cast<vk::DeviceSize>(m_active_ids.size()) * sizeof(ActiveIdEntry);
			m_active_id_staging[current_frame]->writeToBuffer(
				m_active_ids.data(), id_size, 0);
			vk::BufferCopy id_copy{0, 0, id_size};
			cmd.copyBuffer(*m_active_id_staging[current_frame]->getBuffer(),
			               *m_active_id_buffers[current_frame]->getBuffer(),
			               id_copy);
			did_object_copy = true;
		}

		// Upload draw group GPU data
		if (m_total_groups > 0) {
			// DrawGroupGPU data
			std::vector<DrawGroupGPU> gpu_groups(m_total_groups);
			for (uint32_t g = 0; g < m_total_groups; g++) {
				gpu_groups[g].instance_base = m_draw_groups[g].instance_base;
				gpu_groups[g].max_instances = m_draw_groups[g].max_instances;
			}
			vk::DeviceSize group_size = static_cast<vk::DeviceSize>(m_total_groups) * sizeof(DrawGroupGPU);
			m_draw_group_staging[current_frame]->writeToBuffer(
				gpu_groups.data(), group_size, 0);
			vk::BufferCopy group_copy{0, 0, group_size};
			cmd.copyBuffer(*m_draw_group_staging[current_frame]->getBuffer(),
			               *m_draw_group_buffers[current_frame]->getBuffer(),
			               group_copy);

			// Pre-filled indirect commands (instanceCount=0, rest from draw group)
			std::vector<VkDrawIndexedIndirectCommand> cmds(m_total_groups);
			for (uint32_t g = 0; g < m_total_groups; g++) {
				cmds[g].indexCount = m_draw_groups[g].index_count;
				cmds[g].instanceCount = 0;
				cmds[g].firstIndex = m_draw_groups[g].first_index;
				cmds[g].vertexOffset = m_draw_groups[g].vertex_offset;
				cmds[g].firstInstance = m_draw_groups[g].instance_base;
			}
			vk::DeviceSize cmd_size = static_cast<vk::DeviceSize>(m_total_groups) * sizeof(VkDrawIndexedIndirectCommand);
			m_indirect_staging[current_frame]->writeToBuffer(
				cmds.data(), cmd_size, 0);
			did_object_copy = true;
		}

		m_object_data_dirty[current_frame] = false;
	}

	// Copy staging device-local template (only on draw group rebuild)
	bool wrote_template = false;
	if (m_template_needs_copy[current_frame] && m_total_groups > 0) {
		vk::DeviceSize tmpl_size = static_cast<vk::DeviceSize>(m_total_groups) * sizeof(VkDrawIndexedIndirectCommand);
		vk::BufferCopy tmpl_copy{0, 0, tmpl_size};
		cmd.copyBuffer(*m_indirect_staging[current_frame]->getBuffer(),
		               *m_indirect_template[current_frame]->getBuffer(), tmpl_copy);
		m_template_needs_copy[current_frame] = false;
		wrote_template = true;
	}

	// Barrier: transfer writes to compute shader reads (+ transfer reads when template was written)
	if (!copy_regions.empty() || did_object_copy || wrote_template) {
		auto dst_stage = wrote_template
			? (vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer)
			: vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eComputeShader);
		auto dst_access = wrote_template
			? (vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eTransferRead)
			: vk::AccessFlags2(vk::AccessFlagBits2::eShaderStorageRead);
		vk::MemoryBarrier2 barrier{
			.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
			.dstStageMask = dst_stage,
			.dstAccessMask = dst_access,
		};
		vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
		cmd.pipelineBarrier2(dep);
	}

	m_buffer_last_written[current_frame] = m_global_frame_counter;
}

void GpuSceneManager::registerAllObjects(Registry& registry, const PbrMegaBuffer& mega_buffer,
                                          MaterialSSBOManager& mat_mgr) {
	m_mega_buffer = &mega_buffer;
	m_mat_mgr = &mat_mgr;
	auto& mesh_pool = registry.meshes();
	for (uint32_t i = 0; i < mesh_pool.size(); i++) {
		uint32_t entity_idx = mesh_pool.entityAt(i);
		Entity entity = registry.entityFromIndex(entity_idx);
		auto& mesh = mesh_pool.data()[i];
		if (!mesh.hasMesh())
			continue;
		if (!mega_buffer.getEntry(mesh.getMesh()))
			continue;
		registerObject(entity, mesh, mega_buffer, mat_mgr, registry);
	}
	VE_LOGI("GpuSceneManager: registered " << m_active_count << " objects from scene");
}

void GpuSceneManager::unsubscribeFromRegistry() {
	if (!m_registry)
		return;
	auto& events = m_registry->events();
	events.unsubscribe<ComponentRemovedEvent<MeshComponent>>(m_mesh_removed_sub);
	events.unsubscribe<TransformInvalidatedEvent>(m_transform_invalidated_sub);
	events.unsubscribe<MeshDataChangedEvent>(m_mesh_data_changed_sub);
	events.unsubscribe<ComponentAddedEvent<MeshComponent>>(m_mesh_added_sub);
	m_mesh_removed_sub = 0;
	m_transform_invalidated_sub = 0;
	m_mesh_data_changed_sub = 0;
	m_mesh_added_sub = 0;
}

void GpuSceneManager::reset() {
	unsubscribeFromRegistry();
	m_mega_buffer = nullptr;
	m_mat_mgr = nullptr;
	m_registry = nullptr;
	m_entity_to_gpu_id.clear();
	m_gpu_id_to_entity.clear();
	m_free_list.clear();
	m_active_ids.clear();
	m_draw_groups.clear();
	m_total_groups = 0;
	m_transparent_entity_indices.clear();
	std::fill(m_is_transparent_by_gpu_id.begin(), m_is_transparent_by_gpu_id.end(), false);
	for (uint32_t b = 0; b < GPU_CULL_BUCKET_COUNT; b++) {
		m_bucket_group_offsets[b] = 0;
		m_bucket_group_counts[b] = 0;
	}
	m_next_id = 0;
	m_active_count = 0;
	m_object_data_dirty.fill(false);
	m_template_needs_copy.fill(false);
	m_draw_groups_dirty = false;
	std::fill(m_dirty_frame.begin(), m_dirty_frame.end(), 0);
	for (auto& arr : m_object_lod_group_ids)
		arr.fill(0);
	std::fill(m_buffer_last_written.begin(), m_buffer_last_written.end(), 0);
	m_global_frame_counter = 0;
}

uint32_t GpuSceneManager::allocateGpuId() {
	if (!m_free_list.empty()) {
		uint32_t id = m_free_list.back();
		m_free_list.pop_back();
		return id;
	}
	if (m_next_id >= MAX_GPU_OBJECTS)
		VE_LOGE("GPU object limit reached");
	assert(m_next_id < MAX_GPU_OBJECTS && "GPU object limit reached");
	return m_next_id++;
}

void GpuSceneManager::freeGpuId(uint32_t id) {
	m_free_list.push_back(id);
}

void GpuSceneManager::rebuildDrawGroups() {
	m_draw_groups.clear();
	m_active_ids.clear();
	m_active_ids.reserve(m_active_count);

	if (m_active_count == 0) {
		m_total_groups = 0;
		for (uint32_t b = 0; b < GPU_CULL_BUCKET_COUNT; b++) {
			m_bucket_group_offsets[b] = 0;
			m_bucket_group_counts[b] = 0;
		}
		return;
	}

	// Expand each object into one LodVariant per LOD level.
	// Different LOD levels have different (first_index, index_count) so they naturally
	// sort into separate draw groups.
	struct LodVariant {
		uint32_t gpu_id;
		uint32_t lod_level;
		uint32_t vertex_offset;
		uint32_t first_index;
		uint32_t index_count;
		uint32_t material_index;
		uint32_t material_flags;
		uint32_t bucket;
	};

	std::vector<LodVariant> variants;
	variants.reserve(m_active_count * MAX_LOD_LEVELS);

	std::vector<uint32_t> active_gpu_ids;
	active_gpu_ids.reserve(m_active_count);

	for (auto& [entity_idx, gpu_id] : m_entity_to_gpu_id) {
		const CpuLodData& cpu = m_cpu_lod_data[gpu_id];
		bool is_transparent = (cpu.material_flags & 0x20) != 0;
		if (is_transparent)
			continue;

		bool is_mask = (cpu.material_flags & 0x3) == 1;
		bool is_double_sided = (cpu.material_flags & 0x4) != 0;
		uint32_t bucket = (is_mask ? 2u : 0u) + (is_double_sided ? 1u : 0u);
		active_gpu_ids.push_back(gpu_id);

		for (uint32_t l = 0; l < cpu.lod_count; l++) {
			variants.push_back({
				.gpu_id = gpu_id,
				.lod_level = l,
				.vertex_offset = cpu.vertex_offset,
				.first_index = cpu.first_index[l],
				.index_count = cpu.index_count[l],
				.material_index = cpu.material_index,
				.material_flags = cpu.material_flags,
				.bucket = bucket,
			});
		}
	}

	// Sort variants. Objects sharing the same geometry+material at the same LOD group together.
	std::sort(variants.begin(), variants.end(), [](const LodVariant& a, const LodVariant& b) {
		return std::tie(a.bucket, a.vertex_offset, a.first_index, a.index_count, a.material_index)
		     < std::tie(b.bucket, b.vertex_offset, b.first_index, b.index_count, b.material_index);
	});

	// Build draw groups from consecutive variants with the same key
	for (size_t i = 0; i < variants.size(); ) {
		size_t j = i + 1;
		while (j < variants.size()
			&& variants[j].bucket == variants[i].bucket
			&& variants[j].vertex_offset == variants[i].vertex_offset
			&& variants[j].first_index == variants[i].first_index
			&& variants[j].index_count == variants[i].index_count
			&& variants[j].material_index == variants[i].material_index)
			j++;

		DrawGroup group{};
		group.index_count = variants[i].index_count;
		group.first_index = variants[i].first_index;
		group.vertex_offset = static_cast<int32_t>(variants[i].vertex_offset);
		group.material_index = variants[i].material_index;
		group.material_flags = variants[i].material_flags;
		group.bucket = variants[i].bucket;
		group.instance_base = 0;
		group.max_instances = static_cast<uint32_t>(j - i);

		uint32_t group_id = static_cast<uint32_t>(m_draw_groups.size());
		for (size_t k = i; k < j; k++)
			m_object_lod_group_ids[variants[k].gpu_id][variants[k].lod_level] = group_id;

		m_draw_groups.push_back(group);
		i = j;
	}

	m_total_groups = static_cast<uint32_t>(m_draw_groups.size());
	assert(m_total_groups <= MAX_DRAW_GROUPS && "Draw group limit exceeded");

	// Prefix sum: instance_base for each group
	uint32_t running_base = 0;
	for (auto& group : m_draw_groups) {
		group.instance_base = running_base;
		running_base += group.max_instances;
	}

	// Per-bucket offsets and counts
	for (uint32_t b = 0; b < GPU_CULL_BUCKET_COUNT; b++) {
		m_bucket_group_offsets[b] = 0;
		m_bucket_group_counts[b] = 0;
	}
	for (uint32_t g = 0; g < m_total_groups; g++) {
		uint32_t b = m_draw_groups[g].bucket;
		if (m_bucket_group_counts[b] == 0)
			m_bucket_group_offsets[b] = g;
		m_bucket_group_counts[b]++;
	}

	// Patch lod_draw_group_id[] in both frame stagings for every active object.
	// Unused LOD slots (l >= lod_count) duplicate the last valid LOD's group ID.
	static constexpr vk::DeviceSize offset =
		offsetof(ObjectDataGPU, lod_draw_group_id);
	for (uint32_t gpu_id : active_gpu_ids) {
		const CpuLodData& cpu = m_cpu_lod_data[gpu_id];
		uint32_t group_ids[MAX_LOD_LEVELS];
		for (uint32_t l = 0; l < MAX_LOD_LEVELS; l++) {
			uint32_t src = (l < cpu.lod_count) ? l : cpu.lod_count - 1;
			group_ids[l] = m_object_lod_group_ids[gpu_id][src];
		}
		vk::DeviceSize obj_offset = static_cast<vk::DeviceSize>(gpu_id) * sizeof(ObjectDataGPU);
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++)
			m_object_data_staging[f]->writeToBuffer(group_ids, sizeof(group_ids),
			                                        obj_offset + offset);
	}

	// Build active IDs: one entry per non-transparent object
	for (uint32_t gpu_id : active_gpu_ids)
		m_active_ids.push_back(ActiveIdEntry{.gpu_id = gpu_id});

	VE_LOGD("GpuSceneManager: rebuilt " << m_total_groups << " draw groups ("
		<< variants.size() << " LOD variants) from " << m_active_ids.size() << " active objects");
}

void GpuSceneManager::writeObjectData(uint32_t gpu_id, const MeshComponent& mesh,
                                       const PbrMegaBuffer& mega_buffer,
                                       MaterialSSBOManager& mat_mgr) {
	const auto* entry = mega_buffer.getEntry(mesh.getMesh());
	if (!entry)
		return;

	VeMesh::AABB aabb = mesh.getMesh()->getLocalAABB();
	glm::vec3 center = (aabb.min + aabb.max) * 0.5f;
	float radius = glm::length(aabb.max - center);

	uint32_t mat_index = 0;
	uint32_t mat_flags = 0;
	bool is_transparent = false;
	if (mesh.hasMaterial()) {
		mat_index = mat_mgr.registerMaterial(mesh.getMaterial());
		mat_mgr.updateMaterial(mat_index, mesh.getMaterial());
		auto alpha = mesh.getMaterial()->getAlphaProps();
		float transmission = mesh.getMaterial()->getMaterialFactors().transmission_factor;
		is_transparent = (alpha.alpha_mode == AlphaMode::BLEND) || (transmission > 0.0f);
		mat_flags = static_cast<uint32_t>(alpha.alpha_mode)
			| (alpha.double_sided ? 4u : 0u)
			| (mesh.getMaterial()->getFlipTexCoordV() ? 8u : 0u)
			| (alpha.use_spec_gloss_texture ? 16u : 0u)
			| (is_transparent ? 32u : 0u)
			| (!mesh.has_shadow ? 64u : 0u);
	}

	bool was_transparent = m_is_transparent_by_gpu_id[gpu_id];
	if (was_transparent != is_transparent) {
		uint32_t entity_idx = m_gpu_id_to_entity[gpu_id].index();
		if (is_transparent) {
			m_transparent_entity_indices.push_back(entity_idx);
		} else {
			auto it = std::find(m_transparent_entity_indices.begin(),
			                    m_transparent_entity_indices.end(), entity_idx);
			if (it != m_transparent_entity_indices.end()) {
				*it = m_transparent_entity_indices.back();
				m_transparent_entity_indices.pop_back();
			}
		}
		m_is_transparent_by_gpu_id[gpu_id] = is_transparent;
	}

	// Cache per-LOD geometry on CPU side for use in rebuildDrawGroups()
	CpuLodData& cpu = m_cpu_lod_data[gpu_id];
	cpu.vertex_offset = entry->vertex_offset;
	cpu.material_flags = mat_flags;
	cpu.material_index = mat_index;
	cpu.lod_count = static_cast<uint32_t>(entry->lod_entries.size());
	for (uint32_t l = 0; l < MAX_LOD_LEVELS; l++) {
		uint32_t src = (l < entry->lod_entries.size()) ? l
		             : static_cast<uint32_t>(entry->lod_entries.size()) - 1;
		cpu.first_index[l] = entry->lod_entries[src].first_index;
		cpu.index_count[l] = entry->lod_entries[src].index_count;
	}

	// Write GPU struct. lod_draw_group_id[] starts as zero and is patched by rebuildDrawGroups().
	ObjectDataGPU obj{};
	obj.bounding_sphere = glm::vec4(center, radius);
	obj.vertex_offset = entry->vertex_offset;
	obj.material_index = mat_index;
	obj.material_flags = mat_flags;
	obj.lod_count = cpu.lod_count;
	for (uint32_t l = 0; l < MAX_LOD_LEVELS; l++)
		obj.lod_index_count[l] = cpu.index_count[l];

	vk::DeviceSize offset = static_cast<vk::DeviceSize>(gpu_id) * sizeof(ObjectDataGPU);
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++)
		m_object_data_staging[f]->writeToBuffer(&obj, sizeof(ObjectDataGPU), offset);
}

void GpuSceneManager::writeTransform(uint32_t gpu_id, uint32_t frame,
                                      const Registry& registry, Entity entity) {
	const glm::mat4& world = registry.getWorldTransform(entity);
	const glm::mat3& normal = registry.getWorldNormal(entity);

	TransformGPU xform{};
	xform.world_transform = world;
	xform.normal_col0 = glm::vec4(normal[0], 0.0f);
	xform.normal_col1 = glm::vec4(normal[1], 0.0f);
	xform.normal_col2 = glm::vec4(normal[2], 0.0f);

	vk::DeviceSize offset = static_cast<vk::DeviceSize>(gpu_id) * sizeof(TransformGPU);
	m_transform_staging[frame]->writeToBuffer(&xform, sizeof(TransformGPU), offset);
}

} // namespace ve
