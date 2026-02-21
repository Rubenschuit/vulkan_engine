#include "pch.hpp"
#include "rendering/culling_system.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_mesh.hpp"
#include "utils/ve_frustum.hpp"
#include "ve_config.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace ve {

CullingSystem::CullingSystem(VeCamera& camera)
	: m_camera(&camera) {
}

void CullingSystem::cullObjects(VeFrameInfo& frame_info) {
	m_visible_objects.clear();
	m_last_total_mesh_objects = 0;
	m_last_visible_count = 0;

	FrustumPlane planes[6];
	if (m_culling_enabled) {
		m_camera->updateIfDirty();
		const glm::mat4 view_proj = m_camera->getProj() * m_camera->getView();
		extractFrustumPlanes(view_proj, planes);
	}

	const glm::vec3 cam_pos = m_camera->getPosition();
	const float half_tan_fov = std::tan(m_camera->getFovY() * 0.5f);

	auto& registry = *frame_info.registry;
	auto& mesh_pool = registry.meshes();
	m_visible_objects.reserve(mesh_pool.size());
	for (uint32_t i = 0; i < mesh_pool.size(); i++) {
		MeshComponent& mesh = mesh_pool.data()[i];
		if (!mesh.hasMesh() || !mesh.hasMaterial())
			continue;

		uint32_t entity_idx = mesh_pool.entityAt(i);
		Entity entity = registry.entityFromIndex(entity_idx);
		if (!registry.isActive(entity)) continue;

		m_last_total_mesh_objects++;

		const VeMesh::AABB world_aabb = mesh.getWorldAABB();

		if (m_culling_enabled && !isAABBInFrustum(world_aabb, planes))
			continue;

		// LOD selection based on screen-space projected size with hysteresis
		uint32_t lod = 0;
		VeMesh* mesh_ptr = mesh.getMesh();
		if (m_force_lod >= 0) {
			// Forced LOD: clamp to available LODs
			lod = std::min(static_cast<uint32_t>(m_force_lod), mesh_ptr ? mesh_ptr->getLodCount() - 1 : 0u);
			mesh.cached_lod = lod;
		} else if (mesh_ptr && mesh_ptr->getLodCount() > 1) {
			glm::vec3 center = (world_aabb.min + world_aabb.max) * 0.5f;
			float radius = glm::length(world_aabb.max - world_aabb.min) * 0.5f;
			float dist = glm::length(center - cam_pos);
			if (dist > 0.001f) {
				float screen_fraction = radius / (dist * half_tan_fov);
				uint32_t prev_lod = mesh.cached_lod;
				for (uint32_t t = 0; t < ve::MAX_LOD_LEVELS - 1 && t < mesh_ptr->getLodCount() - 1; t++) {
					float threshold = m_lod_thresholds[t];
					if (prev_lod > t)
						threshold *= (1.0f + m_lod_hysteresis);
					if (screen_fraction < threshold)
						lod = t + 1;
				}
				mesh.cached_lod = lod;
			}
		}

		m_visible_objects.push_back({entity, &mesh, lod});
		m_last_visible_count++;
	}
}

} // namespace ve
