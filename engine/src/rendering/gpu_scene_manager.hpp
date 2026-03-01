#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_buffer.hpp"
#include "scene/ve_entity.hpp"
#include "scene/ve_event.hpp"

#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace ve {

class VeDevice;
class Registry;
class MeshComponent;
class PbrMegaBuffer;
class MaterialSSBOManager;

// Updated on entity add/remove/material change. Rarely changes at runtime.
struct ObjectDataGPU {
	glm::vec4 bounding_sphere; // xyz = local-space center, w = radius
	uint32_t vertex_offset;
	uint32_t material_index;
	uint32_t material_flags; // bits 0-1: alpha_mode, bit 2: double_sided, ...
	uint32_t lod_count;
	uint32_t lod_draw_group_id[MAX_LOD_LEVELS]; // draw group per LOD; filled by rebuildDrawGroups()
	uint32_t lod_index_count[MAX_LOD_LEVELS];   // index count per LOD (kept for reference)
};
static_assert(sizeof(ObjectDataGPU) == 64, "ObjectDataGPU must be 64 bytes");

// GPU-resident per-object transform
struct TransformGPU {
	glm::mat4 world_transform;
	glm::vec4 normal_col0;
	glm::vec4 normal_col1;
	glm::vec4 normal_col2;
};
static_assert(sizeof(TransformGPU) == 112, "TransformGPU must be 112 bytes");

// CPU-side draw group: objects with same mesh+material+bucket share one indirect draw
struct DrawGroup {
	uint32_t index_count;
	uint32_t first_index;
	int32_t  vertex_offset;
	uint32_t material_index;
	uint32_t material_flags;
	uint32_t bucket;
	uint32_t instance_base;  // start index in instance buffer
	uint32_t max_instances;  // number of objects in this group
};

// GPU-side draw group info (read by cull shader to place instance data)
struct DrawGroupGPU {
	uint32_t instance_base;
	uint32_t max_instances;
};
static_assert(sizeof(DrawGroupGPU) == 8, "DrawGroupGPU must be 8 bytes");

// Active object entry: one per non-transparent object; shader looks up LOD draw group from ObjectDataGPU
struct ActiveIdEntry {
	uint32_t gpu_id;
	uint32_t _pad{};
};
static_assert(sizeof(ActiveIdEntry) == 8, "ActiveIdEntry must be 8 bytes");

// CPU-side per-object LOD data; cached so rebuildDrawGroups() doesn't need to read GPU staging.
struct CpuLodData {
	uint32_t vertex_offset;
	uint32_t material_flags;
	uint32_t material_index;
	uint32_t lod_count;
	uint32_t first_index[MAX_LOD_LEVELS];
	uint32_t index_count[MAX_LOD_LEVELS];
};

// Manages the GPU-resident scene description: object metadata + transforms.
// Assigns stable GPU object IDs via free-list (ComponentPool dense indices are unstable).
// Subscribes to Registry events for automatic registration/unregistration.
class VENGINE_API GpuSceneManager {
public:
	GpuSceneManager(VeDevice& device);
	~GpuSceneManager();

	GpuSceneManager(const GpuSceneManager&) = delete;
	GpuSceneManager& operator=(const GpuSceneManager&) = delete;

	// Call once when scene is loaded.
	void subscribeToRegistry(Registry& registry);

	// Register a mesh entity for GPU culling. Returns the GPU object ID.
	uint32_t registerObject(Entity entity, const MeshComponent& mesh,
	                        const PbrMegaBuffer& mega_buffer,
	                        MaterialSSBOManager& mat_mgr,
	                        const Registry& registry);

	void unregisterObject(Entity entity);
	void markTransformDirty(Entity entity);
	void markObjectDataDirty(Entity entity);
	void updateDirtyTransforms(uint32_t current_frame, const Registry& registry,
	                           vk::raii::CommandBuffer& cmd);

	// Register all existing mesh entities in the registry
	void registerAllObjects(Registry& registry, const PbrMegaBuffer& mega_buffer,
	                        MaterialSSBOManager& mat_mgr);

	// Clear all registrations (for scene changes).
	void reset();

	uint32_t getObjectCount() const { return static_cast<uint32_t>(m_active_ids.size()); }
	uint32_t getTotalRegisteredCount() const { return m_active_count; }
	bool hasRegisteredObjects() const { return m_active_count > 0; }
	uint32_t getDispatchCount() const { return m_next_id; }

	// Buffer accessors for descriptor set creation
	VeBuffer& getObjectDataBuffer(uint32_t frame) { return *m_object_data_buffers[frame]; }
	VeBuffer& getTransformBuffer(uint32_t frame) { return *m_transform_buffers[frame]; }
	VeBuffer& getActiveIdBuffer(uint32_t frame) { return *m_active_id_buffers[frame]; }
	VeBuffer& getDrawGroupBuffer(uint32_t frame) { return *m_draw_group_buffers[frame]; }
	VeBuffer& getIndirectTemplateBuffer(uint32_t frame) { return *m_indirect_template[frame]; }

	const std::vector<uint32_t>& getTransparentEntityIndices() const { return m_transparent_entity_indices; }

	// Draw group accessors
	uint32_t getTotalGroups() const { return m_total_groups; }
	uint32_t getBucketGroupOffset(uint32_t bucket) const { return m_bucket_group_offsets[bucket]; }
	uint32_t getBucketGroupCount(uint32_t bucket) const { return m_bucket_group_counts[bucket]; }
	const uint32_t* getBucketGroupOffsets() const { return m_bucket_group_offsets; }
	const uint32_t* getBucketGroupCounts() const { return m_bucket_group_counts; }

	bool hasGpuId(Entity entity) const {
		return m_entity_to_gpu_id.find(entity.index()) != m_entity_to_gpu_id.end();
	}

	uint32_t getGpuId(Entity entity) const {
		auto it = m_entity_to_gpu_id.find(entity.index());
		assert(it != m_entity_to_gpu_id.end());
		return it->second;
	}

private:
	uint32_t allocateGpuId();
	void freeGpuId(uint32_t id);

	void writeObjectData(uint32_t gpu_id, const MeshComponent& mesh,
	                     const PbrMegaBuffer& mega_buffer,
	                     MaterialSSBOManager& mat_mgr);

	void writeTransform(uint32_t gpu_id, uint32_t frame, const Registry& registry,
	                    Entity entity);

	void rebuildDrawGroups();

	VeDevice& m_ve_device;
	const PbrMegaBuffer* m_mega_buffer = nullptr;
	MaterialSSBOManager* m_mat_mgr = nullptr;
	Registry* m_registry = nullptr;

	// GPU Object ID management
	uint32_t m_next_id = 0;
	uint32_t m_active_count = 0;
	std::vector<uint32_t> m_free_list;
	std::unordered_map<uint32_t, uint32_t> m_entity_to_gpu_id; // entity_index to gpu_id
	std::unordered_map<uint32_t, Entity> m_gpu_id_to_entity;   // gpu_id to entity

	// GPU buffers (device-local, fast compute reads)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_object_data_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_transform_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_active_id_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_draw_group_buffers;

	// Staging buffers (host-visible, for CPU→GPU copies)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_object_data_staging;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_transform_staging;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_active_id_staging;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_draw_group_staging;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_indirect_staging;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_indirect_template;

	// Active object entries (contiguous, rebuilt on register/unregister)
	std::vector<ActiveIdEntry> m_active_ids;

	// Draw groups (rebuilt on register/unregister)
	std::vector<DrawGroup> m_draw_groups;
	uint32_t m_bucket_group_offsets[GPU_CULL_BUCKET_COUNT]{};
	uint32_t m_bucket_group_counts[GPU_CULL_BUCKET_COUNT]{};
	uint32_t m_total_groups = 0;
	// Per gpu_id, per lod_level: which draw group to route to; filled by rebuildDrawGroups()
	std::vector<std::array<uint32_t, MAX_LOD_LEVELS>> m_object_lod_group_ids;
	// CPU-side LOD geometry cache; filled by writeObjectData() for use in rebuildDrawGroups()
	std::vector<CpuLodData> m_cpu_lod_data;

	// Per GPU object: frame number when transform was last dirtied
	std::vector<uint32_t> m_dirty_frame; // indexed by gpu_id
	// Per frame buffer: last global frame number that was fully written
	std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_buffer_last_written{};
	uint32_t m_global_frame_counter = 0;
	std::array<bool, MAX_FRAMES_IN_FLIGHT> m_object_data_dirty{};
	std::array<bool, MAX_FRAMES_IN_FLIGHT> m_template_needs_copy{};
	bool m_draw_groups_dirty = false;

	// Pre-filtered list of entity indices that have transparent/blend materials.
	// Updated on register/unregister/material-change; consumed by prepareTransparents.
	std::vector<uint32_t> m_transparent_entity_indices;
	std::vector<bool>     m_is_transparent_by_gpu_id; // indexed by gpu_id

	void unsubscribeFromRegistry();

	// Event subscriptions
	SubscriptionId m_mesh_removed_sub = 0;
	SubscriptionId m_transform_invalidated_sub = 0;
	SubscriptionId m_mesh_data_changed_sub = 0;
	SubscriptionId m_mesh_added_sub = 0;
};

} // namespace ve
