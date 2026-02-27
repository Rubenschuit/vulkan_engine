#include "pch.hpp"
#include "rendering/gpu_scene_manager.hpp"
#include "rendering/pbr_mega_buffer.hpp"
#include "rendering/material_ssbo_manager.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"
#include "utils/ve_log.hpp"

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
	}

	m_dirty_frame.resize(MAX_GPU_OBJECTS, 0);
}

GpuSceneManager::~GpuSceneManager() = default;

void GpuSceneManager::subscribeToRegistry(Registry& registry) {
	m_mesh_removed_sub = registry.events().subscribe<ComponentRemovedEvent<MeshComponent>>(
		[this](const ComponentRemovedEvent<MeshComponent>& event) {
			unregisterObject(event.entity);
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

	VE_LOGD("GpuSceneManager: registered entity " << entity.index() << " as GPU object " << gpu_id << " (total: " << m_active_count << ")");
	return gpu_id;
}

void GpuSceneManager::unregisterObject(Entity entity) {
	auto it = m_entity_to_gpu_id.find(entity.index());
	if (it == m_entity_to_gpu_id.end())
		return;

	uint32_t gpu_id = it->second;
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
}

void GpuSceneManager::markTransformDirty(Entity entity) {
	auto it = m_entity_to_gpu_id.find(entity.index());
	if (it == m_entity_to_gpu_id.end())
		return;
	m_dirty_frame[it->second] = m_global_frame_counter;
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

	// Copy object data if structure changed (add/remove)
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
		m_object_data_dirty[current_frame] = false;
	}

	// Barrier: transfer writes to compute shader reads
	if (!copy_regions.empty() || did_object_copy) {
		vk::MemoryBarrier2 barrier{
			.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
		};
		vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
		cmd.pipelineBarrier2(dep);
	}

	m_buffer_last_written[current_frame] = m_global_frame_counter;
}

void GpuSceneManager::registerAllObjects(Registry& registry, const PbrMegaBuffer& mega_buffer,
                                          MaterialSSBOManager& mat_mgr) {
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

void GpuSceneManager::reset() {
	m_entity_to_gpu_id.clear();
	m_gpu_id_to_entity.clear();
	m_free_list.clear();
	m_next_id = 0;
	m_active_count = 0;
	m_object_data_dirty.fill(false);
	std::fill(m_dirty_frame.begin(), m_dirty_frame.end(), 0);
	std::fill(m_buffer_last_written.begin(), m_buffer_last_written.end(), 0);
	m_global_frame_counter = 0;
}

uint32_t GpuSceneManager::allocateGpuId() {
	if (!m_free_list.empty()) {
		uint32_t id = m_free_list.back();
		m_free_list.pop_back();
		return id;
	}
	assert(m_next_id < MAX_GPU_OBJECTS && "GPU object limit reached");
	return m_next_id++;
}

void GpuSceneManager::freeGpuId(uint32_t id) {
	m_free_list.push_back(id);
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
	if (mesh.hasMaterial()) {
		mat_index = mat_mgr.registerMaterial(mesh.getMaterial());
		auto alpha = mesh.getMaterial()->getAlphaProps();
		float transmission = mesh.getMaterial()->getMaterialFactors().transmission_factor;
		bool is_transparent = (alpha.alpha_mode == AlphaMode::BLEND) || (transmission > 0.0f);
		mat_flags = static_cast<uint32_t>(alpha.alpha_mode)
			| (alpha.double_sided ? 4u : 0u)
			| (mesh.getMaterial()->getFlipTexCoordV() ? 8u : 0u)
			| (alpha.use_spec_gloss_texture ? 16u : 0u)
			| (is_transparent ? 32u : 0u);
	}

	ObjectDataGPU obj{};
	obj.bounding_sphere = glm::vec4(center, radius);
	obj.vertex_offset = entry->vertex_offset;
	obj.material_index = mat_index;
	obj.material_flags = mat_flags;
	obj.lod_count = static_cast<uint32_t>(entry->lod_entries.size());

	for (uint32_t l = 0; l < MAX_LOD_LEVELS; l++) {
		if (l < entry->lod_entries.size()) {
			obj.lod_first_index[l] = entry->lod_entries[l].first_index;
			obj.lod_index_count[l] = entry->lod_entries[l].index_count;
		} else {
			uint32_t last = static_cast<uint32_t>(entry->lod_entries.size()) - 1;
			obj.lod_first_index[l] = entry->lod_entries[last].first_index;
			obj.lod_index_count[l] = entry->lod_entries[last].index_count;
		}
	}

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
