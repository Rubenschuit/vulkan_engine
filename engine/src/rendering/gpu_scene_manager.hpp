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
	uint32_t lod_first_index[MAX_LOD_LEVELS];
	uint32_t lod_index_count[MAX_LOD_LEVELS];
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
	void updateDirtyTransforms(uint32_t current_frame, const Registry& registry,
	                           vk::raii::CommandBuffer& cmd);

	// Register all existing mesh entities in the registry
	void registerAllObjects(Registry& registry, const PbrMegaBuffer& mega_buffer,
	                        MaterialSSBOManager& mat_mgr);

	// Clear all registrations (for scene changes).
	void reset();

	uint32_t getObjectCount() const { return m_active_count; }
	uint32_t getDispatchCount() const { return m_next_id; }

	// Buffer accessors for descriptor set creation
	VeBuffer& getObjectDataBuffer(uint32_t frame) { return *m_object_data_buffers[frame]; }
	VeBuffer& getTransformBuffer(uint32_t frame) { return *m_transform_buffers[frame]; }

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

	VeDevice& m_ve_device;

	// GPU Object ID management
	uint32_t m_next_id = 0;
	uint32_t m_active_count = 0;
	std::vector<uint32_t> m_free_list;
	std::unordered_map<uint32_t, uint32_t> m_entity_to_gpu_id; // entity_index → gpu_id
	std::unordered_map<uint32_t, Entity> m_gpu_id_to_entity;   // gpu_id → entity

	// GPU buffers (device-local, fast compute reads)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_object_data_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_transform_buffers;

	// Staging buffers (host-visible, for CPU→GPU copies)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_object_data_staging;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_transform_staging;

	// Per GPU object: frame number when transform was last dirtied
	std::vector<uint32_t> m_dirty_frame; // indexed by gpu_id
	// Per frame buffer: last global frame number that was fully written
	std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_buffer_last_written{};
	uint32_t m_global_frame_counter = 0;
	std::array<bool, MAX_FRAMES_IN_FLIGHT> m_object_data_dirty{};

	// Event subscriptions
	SubscriptionId m_mesh_removed_sub = 0;
};

} // namespace ve