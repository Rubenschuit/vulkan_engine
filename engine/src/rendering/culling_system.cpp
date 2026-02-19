#include "pch.hpp"
#include "rendering/culling_system.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_mesh.hpp"
#include "utils/ve_frustum.hpp"

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

		if (!m_culling_enabled) {
			m_visible_objects.push_back({entity, &mesh});
			m_last_visible_count++;
		} else {
			const VeMesh::AABB world_aabb = mesh.getWorldAABB();
			if (isAABBInFrustum(world_aabb, planes)) {
				m_visible_objects.push_back({entity, &mesh});
				m_last_visible_count++;
			}
		}
	}
}

} // namespace ve
