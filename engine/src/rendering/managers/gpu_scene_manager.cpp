#include "pch.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "rendering/culling/meshlet_data.hpp"
#include "rendering/managers/material_ssbo_manager.hpp"
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

		m_meshlet_object_info_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(MeshletObjectInfo), MAX_GPU_OBJECTS,
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

		m_meshlet_object_info_staging[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(MeshletObjectInfo), MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_meshlet_object_info_staging[i]->map();
	}

	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
		m_id_in_dirty_set[f].assign(MAX_GPU_OBJECTS, false);
		m_dirty_ids[f].reserve(1024);
	}
	m_object_lod_group_ids.resize(MAX_GPU_OBJECTS);
	m_cpu_object_data.resize(MAX_GPU_OBJECTS);
	m_meshlet_object_info_cpu.resize(MAX_GPU_OBJECTS);
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

	m_skin_added_sub = registry.events().subscribe<ComponentAddedEvent<SkinComponent>>(
		[this](const ComponentAddedEvent<SkinComponent>& event) {
			if (!m_mega_buffer || !m_mat_mgr || !m_registry)
				return;
			auto* mc = m_registry->getComponent<MeshComponent>(event.entity);
			if (!mc || !mc->hasMesh())
				return;
			if (hasGpuId(event.entity)) {
				markObjectDataDirty(event.entity);
			} else if (m_mega_buffer->getEntry(mc->getMesh())) {
				registerObject(event.entity, *mc, *m_mega_buffer, *m_mat_mgr, *m_registry);
			}
			if (hasGpuId(event.entity))
				setDynamic(event.entity, isDynamicEntity(*m_registry, event.entity));
		});
	m_skin_removed_sub = registry.events().subscribe<ComponentRemovedEvent<SkinComponent>>(
		[this](const ComponentRemovedEvent<SkinComponent>& event) {
			if (!hasGpuId(event.entity) || !m_registry)
				return;
			markObjectDataDirty(event.entity);
			setDynamic(event.entity, isDynamicEntity(*m_registry, event.entity));
		});
	m_morph_added_sub = registry.events().subscribe<ComponentAddedEvent<MorphComponent>>(
		[this](const ComponentAddedEvent<MorphComponent>& event) {
			if (!m_mega_buffer || !m_mat_mgr || !m_registry)
				return;
			auto* mc = m_registry->getComponent<MeshComponent>(event.entity);
			if (!mc || !mc->hasMesh())
				return;
			if (hasGpuId(event.entity)) {
				markObjectDataDirty(event.entity);
			} else if (m_mega_buffer->getEntry(mc->getMesh())) {
				registerObject(event.entity, *mc, *m_mega_buffer, *m_mat_mgr, *m_registry);
			}
			if (hasGpuId(event.entity))
				setDynamic(event.entity, isDynamicEntity(*m_registry, event.entity));
		});
	m_morph_removed_sub = registry.events().subscribe<ComponentRemovedEvent<MorphComponent>>(
		[this](const ComponentRemovedEvent<MorphComponent>& event) {
			if (!hasGpuId(event.entity) || !m_registry)
				return;
			markObjectDataDirty(event.entity);
			setDynamic(event.entity, isDynamicEntity(*m_registry, event.entity));
		});
	m_rb_changed_sub = registry.events().subscribe<RigidbodyChangedEvent>(
		[this](const RigidbodyChangedEvent& event) {
			if (hasGpuId(event.entity) && m_registry)
				setDynamic(event.entity, isDynamicEntity(*m_registry, event.entity));
		});
	m_rb_removed_sub = registry.events().subscribe<ComponentRemovedEvent<RigidbodyComponent>>(
		[this](const ComponentRemovedEvent<RigidbodyComponent>& event) {
			if (hasGpuId(event.entity) && m_registry)
				setDynamic(event.entity, isDynamicEntity(*m_registry, event.entity));
		});
	m_anim_state_changed_sub = registry.events().subscribe<AnimationStateChangedEvent>(
		[this](const AnimationStateChangedEvent& event) {
			if (!m_registry)
				return;
			auto* animator = m_registry->getComponent<AnimatorComponent>(event.entity);
			if (!animator)
				return;
			for (Entity target : animator->getAnimatedEntities()) {
				if (hasGpuId(target))
					setDynamic(target, isDynamicEntity(*m_registry, target));
			}
		});
	m_anim_removed_sub = registry.events().subscribe<ComponentRemovedEvent<AnimatorComponent>>(
		[this](const ComponentRemovedEvent<AnimatorComponent>& event) {
			if (!m_registry)
				return;
			auto* animator = m_registry->getComponent<AnimatorComponent>(event.entity);
			if (!animator)
				return;
			for (Entity target : animator->getAnimatedEntities()) {
				if (!m_registry->isAlive(target))
					continue;
				m_registry->setAnimated(target, false);
				if (hasGpuId(target))
					setDynamic(target, isDynamicEntity(*m_registry, target));
			}
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

	if (isDynamicEntity(registry, entity))
		setDynamic(entity, true);

	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
		if (!m_id_in_dirty_set[f][gpu_id]) {
			m_id_in_dirty_set[f][gpu_id] = true;
			m_dirty_ids[f].push_back(gpu_id);
		}
	}
	m_object_data_dirty.fill(true);
	m_draw_groups_dirty = true;

	return gpu_id;
}

void GpuSceneManager::unregisterObject(Entity entity) {
	auto it = m_entity_to_gpu_id.find(entity.index());
	if (it == m_entity_to_gpu_id.end())
		return;

	uint32_t gpu_id = it->second;

	if (m_cpu_object_data[gpu_id].object_flags & ObjectFlag::DYNAMIC) {
		assert(m_dynamic_object_count > 0 && "DYNAMIC flag set but count is 0");
		if (m_dynamic_object_count > 0)
			m_dynamic_object_count--;
		else
			VE_LOGE("GpuSceneManager: dynamic count underflow on unregister of gpu_id " << gpu_id);
	}

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

	m_cpu_object_data[gpu_id] = {};
	m_meshlet_object_info_cpu[gpu_id] = {};
	m_object_lod_group_ids[gpu_id].fill(0);

	m_object_data_dirty.fill(true);
	m_draw_groups_dirty = true;
}

void GpuSceneManager::markTransformDirty(Entity entity) {
	auto it = m_entity_to_gpu_id.find(entity.index());
	if (it == m_entity_to_gpu_id.end())
		return;
	uint32_t gpu_id = it->second;
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
		if (!m_id_in_dirty_set[f][gpu_id]) {
			m_id_in_dirty_set[f][gpu_id] = true;
			m_dirty_ids[f].push_back(gpu_id);
		}
	}
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
	m_draw_groups_dirty = true;
}

bool GpuSceneManager::isDynamicEntity(const Registry& registry, Entity entity) {
	if (registry.isAnimated(entity))
		return true;
	if (registry.hasComponent<SkinComponent>(entity))
		return true;
	if (registry.hasComponent<MorphComponent>(entity))
		return true;
	const auto* rb = registry.getComponent<RigidbodyComponent>(entity);
	return rb && rb->getMotionType() == PhysicsMotionType::Dynamic;
}

void GpuSceneManager::setDynamic(Entity entity, bool is_dynamic) {
	auto it = m_entity_to_gpu_id.find(entity.index());
	if (it == m_entity_to_gpu_id.end())
		return;
	uint32_t gpu_id = it->second;

	auto& obj_flags = m_cpu_object_data[gpu_id].object_flags;
	bool was_dynamic = (obj_flags & ObjectFlag::DYNAMIC) != 0;
	if (was_dynamic == is_dynamic)
		return;

	if (is_dynamic) {
		obj_flags |= ObjectFlag::DYNAMIC;
		m_dynamic_object_count++;
	} else {
		obj_flags &= ~ObjectFlag::DYNAMIC;
		assert(m_dynamic_object_count > 0 && "DYNAMIC flag cleared but count is 0");
		if (m_dynamic_object_count > 0)
			m_dynamic_object_count--;
		else
			VE_LOGE("GpuSceneManager: dynamic count underflow on setDynamic(false) for gpu_id " << gpu_id);
	}

	m_object_data_dirty.fill(true);
	m_dynamic_classification_changed = true;
}

// Only skinned bounds change per frame. Morph-only meshes keep the
// static conservative bound set at registration (does not account for weights
// exceeding 1.0 for now).
void GpuSceneManager::refreshSkinnedAabbs(Registry& registry) {
	bool any = false;
	auto write_aabb = [&](Entity entity, MeshComponent& mc) {
		auto it = m_entity_to_gpu_id.find(entity.index());
		if (it == m_entity_to_gpu_id.end())
			return;
		uint32_t gpu_id = it->second;
		VeMesh::AABB world_aabb = mc.getWorldAABB();
		VeMesh::AABB local = transformAABB(world_aabb,
			glm::inverse(registry.getWorldTransform(entity)));
		auto& cpu = m_cpu_object_data[gpu_id];
		cpu.aabb_min = local.min;
		cpu.aabb_max = local.max;
		any = true;
	};
	for (auto [entity, mc, sc] : registry.view<MeshComponent, SkinComponent>())
		write_aabb(entity, mc);
	if (any)
		m_object_data_dirty.fill(true);
}

void GpuSceneManager::updateDirtyTransforms(uint32_t current_frame, const Registry& registry,
                                             vk::raii::CommandBuffer& cmd) {
	auto& dirty_ids = m_dirty_ids[current_frame];
	auto& dirty_set = m_id_in_dirty_set[current_frame];

	std::vector<vk::BufferCopy> copy_regions;
	if (!dirty_ids.empty()) {
		// Sort so consecutive gpu_ids coalesce into a single buffer copy.
		std::sort(dirty_ids.begin(), dirty_ids.end());
		copy_regions.reserve(dirty_ids.size());

		for (size_t i = 0; i < dirty_ids.size(); i++) {
			uint32_t gpu_id = dirty_ids[i];
			dirty_set[gpu_id] = false;

			auto entity_it = m_gpu_id_to_entity.find(gpu_id);
			if (entity_it == m_gpu_id_to_entity.end())
				continue;
			Entity entity = entity_it->second;
			if (!registry.isAlive(entity))
				continue;

			writeTransform(gpu_id, current_frame, registry, entity);

			vk::DeviceSize offset = static_cast<vk::DeviceSize>(gpu_id) * sizeof(TransformGPU);
			if (!copy_regions.empty()
			    && copy_regions.back().srcOffset + copy_regions.back().size == offset) {
				copy_regions.back().size += sizeof(TransformGPU);
			} else {
				copy_regions.push_back({offset, offset, sizeof(TransformGPU)});
			}
		}
		dirty_ids.clear();
	}
	if (!copy_regions.empty()) {
		cmd.copyBuffer(m_transform_staging[current_frame]->getBuffer(),
		               m_transform_buffers[current_frame]->getBuffer(),
		               copy_regions);
	}

	// Rebuild draw groups first (updates m_object_lod_group_ids),
	// then materialise staging[current_frame] from CPU caches, then record
	bool did_object_copy = false;
	bool wrote_template = false;
	if (m_object_data_dirty[current_frame]) {
		if (m_draw_groups_dirty) {
			rebuildDrawGroups();
			m_draw_groups_dirty = false;
		}

		stageObjectData(current_frame);

		vk::DeviceSize size = static_cast<vk::DeviceSize>(m_next_id) * sizeof(ObjectDataGPU);
		if (size > 0) {
			cmd.copyBuffer(m_object_data_staging[current_frame]->getBuffer(),
			               m_object_data_buffers[current_frame]->getBuffer(),
			               vk::BufferCopy{0, 0, size});
			did_object_copy = true;
		}

		vk::DeviceSize moi_size = static_cast<vk::DeviceSize>(m_next_id) * sizeof(MeshletObjectInfo);
		if (moi_size > 0) {
			cmd.copyBuffer(m_meshlet_object_info_staging[current_frame]->getBuffer(),
			               m_meshlet_object_info_buffers[current_frame]->getBuffer(),
			               vk::BufferCopy{0, 0, moi_size});
		}

		if (!m_active_ids.empty()) {
			vk::DeviceSize id_size = static_cast<vk::DeviceSize>(m_active_ids.size()) * sizeof(ActiveIdEntry);
			m_active_id_staging[current_frame]->writeToBuffer(
				m_active_ids.data(), id_size, 0);
			vk::BufferCopy id_copy{0, 0, id_size};
			cmd.copyBuffer(m_active_id_staging[current_frame]->getBuffer(),
			               m_active_id_buffers[current_frame]->getBuffer(),
			               id_copy);
			did_object_copy = true;
		}

		if (m_total_groups > 0) {
			std::vector<DrawGroupGPU> gpu_groups(m_total_groups);
			for (uint32_t g = 0; g < m_total_groups; g++) {
				gpu_groups[g].instance_base = m_draw_groups[g].instance_base;
				gpu_groups[g].max_instances = m_draw_groups[g].max_instances;
			}
			vk::DeviceSize group_size = static_cast<vk::DeviceSize>(m_total_groups) * sizeof(DrawGroupGPU);
			m_draw_group_staging[current_frame]->writeToBuffer(
				gpu_groups.data(), group_size, 0);
			vk::BufferCopy group_copy{0, 0, group_size};
			cmd.copyBuffer(m_draw_group_staging[current_frame]->getBuffer(),
			               m_draw_group_buffers[current_frame]->getBuffer(),
			               group_copy);

			// Pre-filled indirect commands. vertexOffset is left zero: the cull shader
			// writes the correct value.
			std::vector<VkDrawIndexedIndirectCommand> cmds(m_total_groups);
			for (uint32_t g = 0; g < m_total_groups; g++) {
				cmds[g].indexCount = m_draw_groups[g].index_count;
				cmds[g].instanceCount = 0;
				cmds[g].firstIndex = m_draw_groups[g].first_index;
				cmds[g].vertexOffset = 0;
				cmds[g].firstInstance = m_draw_groups[g].instance_base;
			}
			vk::DeviceSize cmd_size = static_cast<vk::DeviceSize>(m_total_groups) * sizeof(VkDrawIndexedIndirectCommand);
			m_indirect_staging[current_frame]->writeToBuffer(
				cmds.data(), cmd_size, 0);
			cmd.copyBuffer(m_indirect_staging[current_frame]->getBuffer(),
			               m_indirect_template[current_frame]->getBuffer(),
			               vk::BufferCopy{0, 0, cmd_size});
			did_object_copy = true;
			wrote_template = true;
		}

		m_object_data_dirty[current_frame] = false;
	}

	if (!copy_regions.empty() || did_object_copy) {
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
	events.unsubscribe<ComponentAddedEvent<SkinComponent>>(m_skin_added_sub);
	events.unsubscribe<ComponentRemovedEvent<SkinComponent>>(m_skin_removed_sub);
	events.unsubscribe<ComponentAddedEvent<MorphComponent>>(m_morph_added_sub);
	events.unsubscribe<ComponentRemovedEvent<MorphComponent>>(m_morph_removed_sub);
	events.unsubscribe<RigidbodyChangedEvent>(m_rb_changed_sub);
	events.unsubscribe<ComponentRemovedEvent<RigidbodyComponent>>(m_rb_removed_sub);
	events.unsubscribe<AnimationStateChangedEvent>(m_anim_state_changed_sub);
	events.unsubscribe<ComponentRemovedEvent<AnimatorComponent>>(m_anim_removed_sub);
	m_mesh_removed_sub = 0;
	m_transform_invalidated_sub = 0;
	m_mesh_data_changed_sub = 0;
	m_mesh_added_sub = 0;
	m_skin_added_sub = 0;
	m_skin_removed_sub = 0;
	m_morph_added_sub = 0;
	m_morph_removed_sub = 0;
	m_rb_changed_sub = 0;
	m_rb_removed_sub = 0;
	m_anim_state_changed_sub = 0;
	m_anim_removed_sub = 0;
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
	m_dynamic_object_count = 0;
	m_transparent_entity_indices.clear();
	std::fill(m_is_transparent_by_gpu_id.begin(), m_is_transparent_by_gpu_id.end(), false);
	for (uint32_t b = 0; b < GPU_CULL_BUCKET_COUNT; b++) {
		m_bucket_group_offsets[b] = 0;
		m_bucket_group_counts[b] = 0;
	}
	m_next_id = 0;
	m_active_count = 0;
	m_object_data_dirty.fill(false);
	m_draw_groups_dirty = false;
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
		m_dirty_ids[f].clear();
		std::fill(m_id_in_dirty_set[f].begin(), m_id_in_dirty_set[f].end(), false);
	}
	for (auto& arr : m_object_lod_group_ids)
		arr.fill(0);
	for (auto& cpu : m_cpu_object_data)
		cpu = {};
	for (auto& moi : m_meshlet_object_info_cpu)
		moi = {};
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
		uint32_t material_index; // part of the grouping key only
		uint32_t bucket;
	};

	std::vector<LodVariant> variants;
	variants.reserve(m_active_count * MAX_LOD_LEVELS);

	std::vector<uint32_t> active_gpu_ids;
	active_gpu_ids.reserve(m_active_count);

	auto isSkinned = [this](uint32_t gpu_id) {
		return (m_cpu_object_data[gpu_id].object_flags & ObjectFlag::DEFORMED) != 0;
	};

	for (auto& [entity_idx, gpu_id] : m_entity_to_gpu_id) {
		const CpuObjectData& cpu = m_cpu_object_data[gpu_id];
		bool is_transparent = (cpu.object_flags & ObjectFlag::IS_TRANSPARENT) != 0;
		bool is_mask = (cpu.material_flags & MaterialFlag::ALPHA_MODE_MASK) == 1;
		bool is_double_sided = (cpu.material_flags & MaterialFlag::DOUBLE_SIDED) != 0;

		uint32_t bucket;
		if (is_transparent)
			bucket = 4u + (is_double_sided ? 1u : 0u);
		else
			bucket = (is_mask ? 2u : 0u) + (is_double_sided ? 1u : 0u);
		active_gpu_ids.push_back(gpu_id);

		for (uint32_t l = 0; l < cpu.lod_count; l++) {
			variants.push_back({
				.gpu_id = gpu_id,
				.lod_level = l,
				.vertex_offset = cpu.vertex_offset,
				.first_index = cpu.first_index[l],
				.index_count = cpu.index_count[l],
				.material_index = cpu.material_index,
				.bucket = bucket,
			});
		}
	}

	// Sort variants. Objects sharing the same geometry+material at the same LOD group
	// together; gpu_id is included so skinned variants always form one-entity groups.
	std::sort(variants.begin(), variants.end(), [&isSkinned](const LodVariant& a, const LodVariant& b) {
		uint32_t a_skin_key = isSkinned(a.gpu_id) ? a.gpu_id : 0u;
		uint32_t b_skin_key = isSkinned(b.gpu_id) ? b.gpu_id : 0u;
		return std::tie(a.bucket, a.vertex_offset, a.first_index, a.index_count, a.material_index, a_skin_key)
		     < std::tie(b.bucket, b.vertex_offset, b.first_index, b.index_count, b.material_index, b_skin_key);
	});

	// Build draw groups from consecutive variants with the same key. Skinned variants
	// form one-entity groups: their per-frame vertexOffset is unique per entity and
	// resolved by the cull shader.
	for (size_t i = 0; i < variants.size(); ) {
		size_t j = i + 1;
		bool i_skinned = isSkinned(variants[i].gpu_id);
		while (!i_skinned
			&& j < variants.size()
			&& !isSkinned(variants[j].gpu_id)
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
	if (m_total_groups > MAX_DRAW_GROUPS) {
		VE_LOGE("GpuSceneManager: draw group count " << m_total_groups
			<< " exceeds MAX_DRAW_GROUPS (" << MAX_DRAW_GROUPS
			<< "); truncating tail. Increase MAX_DRAW_GROUPS or reduce mesh/material variety.");
		m_total_groups = MAX_DRAW_GROUPS;
		m_draw_groups.resize(MAX_DRAW_GROUPS);
	}

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

	for (uint32_t gpu_id : active_gpu_ids) {
		const CpuObjectData& cpu = m_cpu_object_data[gpu_id];
		if (cpu.lod_count == 0) {
			VE_LOGW("GpuSceneManager: object with gpu_id " << gpu_id << " has zero LODs, skipping draw group assignment");
			continue;
		}
		auto& groups = m_object_lod_group_ids[gpu_id];
		uint32_t last_valid = groups[cpu.lod_count - 1];
		for (uint32_t l = cpu.lod_count; l < MAX_LOD_LEVELS; l++)
			groups[l] = last_valid;
	}

	// Sort by LOD-0 draw group
	std::sort(active_gpu_ids.begin(), active_gpu_ids.end(),
		[this](uint32_t a, uint32_t b) {
			return m_object_lod_group_ids[a][0] < m_object_lod_group_ids[b][0];
		});

	// Build active IDs: one entry per object (including transparents for WBOIT)
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

	Entity entity = m_gpu_id_to_entity[gpu_id];
	const bool is_skinned = m_registry && m_registry->hasComponent<SkinComponent>(entity)
		&& mesh.getMesh()->hasSkinning();
	const bool is_morph = m_registry && m_registry->hasComponent<MorphComponent>(entity)
		&& mesh.getMesh()->hasMorphTargets();
	const bool is_deformed = is_skinned || is_morph;

	VeMesh::AABB aabb;
	if (is_skinned) {
		const auto& extents = mesh.getMesh()->getJointMeshLocalExtents();
		if (!extents.empty()) {
			aabb = extents.front();
			for (size_t i = 1; i < extents.size(); i++)
				aabb = unionAABB(aabb, extents[i]);
		} else {
			aabb = mesh.getMesh()->getLocalAABB();
		}
		// Skinned + morph: union the morph bound so morph displacement is covered too.
		if (is_morph)
			aabb = unionAABB(aabb, mesh.getMesh()->getMorphLocalAABB());
	} else if (is_morph) {
		aabb = mesh.getMesh()->getMorphLocalAABB();
	} else {
		aabb = mesh.getMesh()->getLocalAABB();
	}

	uint32_t mat_index = 0;
	uint32_t mat_flags = 0;
	uint32_t obj_flags = 0;
	bool is_transparent = false;
	if (mesh.hasMaterial()) {
		mat_index = mat_mgr.indexFor(mesh.getMaterial());
		mat_mgr.updateMaterial(mat_index, mesh.getMaterial());
		auto alpha = mesh.getMaterial()->getAlphaProps();
		float transmission = mesh.getMaterial()->getMaterialFactors().transmission_factor;
		is_transparent = (alpha.alpha_mode == AlphaMode::BLEND) || (transmission > 0.0f);
		mat_flags = static_cast<uint32_t>(alpha.alpha_mode)
			| (alpha.double_sided ? MaterialFlag::DOUBLE_SIDED : 0u)
			| (mesh.getMaterial()->getFlipTexCoordV() ? MaterialFlag::FLIP_TEX_V : 0u)
			| (alpha.use_spec_gloss_texture ? MaterialFlag::SPEC_GLOSS : 0u);
		obj_flags = (is_transparent ? ObjectFlag::IS_TRANSPARENT : 0u)
			| (!mesh.has_shadow ? ObjectFlag::NO_SHADOW : 0u);
	}
	obj_flags |= (m_cpu_object_data[gpu_id].object_flags & ObjectFlag::DYNAMIC);
	if (is_deformed)
		obj_flags |= ObjectFlag::DEFORMED;

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

	CpuObjectData& cpu = m_cpu_object_data[gpu_id];
	cpu.aabb_min = aabb.min;
	cpu.aabb_max = aabb.max;
	cpu.vertex_offset = entry->vertex_offset;
	cpu.material_flags = mat_flags;
	cpu.object_flags = obj_flags;
	cpu.material_index = mat_index;
	cpu.lod_count = is_deformed ? 1u : static_cast<uint32_t>(entry->lod_entries.size());
	for (uint32_t l = 0; l < MAX_LOD_LEVELS; l++) {
		uint32_t src = (l < entry->lod_entries.size()) ? l
		             : static_cast<uint32_t>(entry->lod_entries.size()) - 1;
		cpu.first_index[l] = entry->lod_entries[src].first_index;
		cpu.index_count[l] = entry->lod_entries[src].index_count;
	}

	MeshletObjectInfo& moi = m_meshlet_object_info_cpu[gpu_id];
	moi = {};
	const auto* meshlet_entry = mega_buffer.getMeshletEntry(mesh.getMesh());
	if (meshlet_entry) {
		for (uint32_t l = 0; l < MAX_LOD_LEVELS; l++) {
			if (l < meshlet_entry->lod_entries.size()) {
				moi.meshlet_offset[l] = meshlet_entry->lod_entries[l].meshlet_offset;
				moi.meshlet_count[l]  = meshlet_entry->lod_entries[l].meshlet_count;
			}
		}
	}
}

void GpuSceneManager::buildObjectData(uint32_t gpu_id, ObjectDataGPU& out) const {
	const auto& cpu = m_cpu_object_data[gpu_id];
	out = {};
	out.aabb_min = glm::vec4(cpu.aabb_min, 0.0f);
	out.aabb_max = glm::vec4(cpu.aabb_max, 0.0f);
	out.vertex_offset = cpu.vertex_offset;
	out.material_index = cpu.material_index;
	out.material_flags = cpu.material_flags;
	out.object_flags = cpu.object_flags;
	out.lod_count = cpu.lod_count;
	const auto& lod_groups = m_object_lod_group_ids[gpu_id];
	for (uint32_t l = 0; l < MAX_LOD_LEVELS; l++) {
		out.lod_draw_group_id[l] = lod_groups[l];
		out.lod_index_count[l] = cpu.index_count[l];
	}
}

void GpuSceneManager::stageObjectData(uint32_t frame) {
	if (m_next_id == 0)
		return;
	auto* obj_staging = static_cast<ObjectDataGPU*>(
		m_object_data_staging[frame]->getMappedMemory());
	auto* moi_staging = static_cast<MeshletObjectInfo*>(
		m_meshlet_object_info_staging[frame]->getMappedMemory());
	for (uint32_t id = 0; id < m_next_id; id++) {
		buildObjectData(id, obj_staging[id]);
		moi_staging[id] = m_meshlet_object_info_cpu[id];
	}
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
