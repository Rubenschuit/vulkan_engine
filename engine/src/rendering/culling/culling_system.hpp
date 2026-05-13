#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "scene/ve_entity.hpp"

#include <glm/glm.hpp>
#include <vector>

namespace ve {

struct FrustumPlane;
struct VeFrameInfo;
class VeThreadPool;
class MeshComponent;
class Registry;

// Cached visible object built once per frame by the culling system.
struct VisibleObject {
	Entity entity;
	MeshComponent* mesh = nullptr;
	uint32_t lod_level = 0;
};

class VENGINE_API CullingSystem {
public:
	CullingSystem() = default;
	void setCullingEnabled(bool enabled) { m_culling_enabled = enabled; }
	bool isCullingEnabled() const { return m_culling_enabled; }
	void setForceLodLevel(int level) { m_force_lod = level; }
	void setLodThresholds(const float* thresholds) { for (int i = 0; i < 3; i++) m_lod_thresholds[i] = thresholds[i]; }
	void setLodHysteresis(float h) { m_lod_hysteresis = h; }
	void setMinParallelEntities(uint32_t n) { m_min_parallel_entities = n; }
	void cullObjects(VeFrameInfo& frame_info, VeThreadPool* thread_pool = nullptr);

	std::vector<VisibleObject>& getVisibleObjectsRef() { return m_visible_objects; }

	uint32_t getLastTotalMeshObjects() const { return m_last_total_mesh_objects; }
	uint32_t getLastVisibleCount() const { return m_last_visible_count; }

private:
	struct CullParams {
		Registry* registry;
		const uint32_t* entity_indices;
		const FrustumPlane* planes;
		glm::vec3 cam_pos;
		float half_tan_fov;
	};

	void processEntity(const CullParams& params, uint32_t dense_idx,
		std::vector<VisibleObject>& out, uint32_t& mesh_count);

	bool m_culling_enabled = true;
	int m_force_lod = -1;
	float m_lod_thresholds[3] = {0.3f, 0.15f, 0.05f};
	float m_lod_hysteresis = 0.2f;
	uint32_t m_min_parallel_entities = MIN_PARALLEL_CULL_ENTITIES;
	std::vector<VisibleObject> m_visible_objects;
	uint32_t m_last_total_mesh_objects = 0;
	uint32_t m_last_visible_count = 0;
};
} // namespace ve