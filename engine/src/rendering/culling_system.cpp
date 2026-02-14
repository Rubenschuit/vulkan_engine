#include "pch.hpp"
#include "rendering/culling_system.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace ve {

namespace {

// Frustrum  culling helper functions

struct FrustumPlane {
	glm::vec4 plane; // xyz = normal, w = d (plane: dot(xyz, point) + w = 0)
};

void extractFrustumPlanes(const glm::mat4& view_proj, FrustumPlane planes[6]) {
	const glm::mat4& m = view_proj;
	// Gribb-Hartmann extraction (column-major)
	planes[0].plane = glm::vec4(m[0][0] + m[0][3], m[1][0] + m[1][3], m[2][0] + m[2][3], m[3][0] + m[3][3]); // left
	planes[1].plane = glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]); // right
	planes[2].plane = glm::vec4(m[0][1] + m[0][3], m[1][1] + m[1][3], m[2][1] + m[2][3], m[3][1] + m[3][3]); // bottom
	planes[3].plane = glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]); // top
	planes[4].plane = glm::vec4(m[0][2] + m[0][3], m[1][2] + m[1][3], m[2][2] + m[2][3], m[3][2] + m[3][3]); // near
	planes[5].plane = glm::vec4(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]); // far

	for (int i = 0; i < 6; ++i) {
		glm::vec3 n = glm::vec3(planes[i].plane);
		float len = glm::length(n);
		if (len > 0.0001f) {
			planes[i].plane /= len;
		}
	}
}

bool isAABBOutsidePlane(const VeMesh::AABB& aabb, const glm::vec4& plane) {
	const glm::vec3 n(plane.x, plane.y, plane.z);
	const float d = plane.w;
	glm::vec3 p;
	p.x = (n.x >= 0.0f) ? aabb.max.x : aabb.min.x;
	p.y = (n.y >= 0.0f) ? aabb.max.y : aabb.min.y;
	p.z = (n.z >= 0.0f) ? aabb.max.z : aabb.min.z;
	return glm::dot(n, p) + d < 0.0f;
}

bool isAABBInFrustum(const VeMesh::AABB& aabb, const FrustumPlane planes[6]) {
	for (int i = 0; i < 6; ++i) {
		if (isAABBOutsidePlane(aabb, planes[i].plane))
			return false;
	}
	return true;
}

} // namespace

CullingSystem::CullingSystem(VeCamera& camera)
	: m_camera(&camera) {
}

void CullingSystem::cullObjects(VeFrameInfo& frame_info) {
	m_visible_game_objects.clear();
	m_last_total_mesh_objects = 0;
	m_last_visible_count = 0;

	FrustumPlane planes[6];
	if (m_culling_enabled) {
		m_camera->updateIfDirty();
		const glm::mat4 view_proj = m_camera->getProj() * m_camera->getView();
		extractFrustumPlanes(view_proj, planes);
	}

	for (auto& [id, obj] : frame_info.game_objects) {
		auto* mesh_comp = obj.getComponent<MeshComponent>();
		if (!mesh_comp || !mesh_comp->hasMesh() || !mesh_comp->hasMaterial())
			continue;

		m_last_total_mesh_objects++;

		if (!m_culling_enabled) {
			m_visible_game_objects[id] = VisibleObject{&obj, mesh_comp};
			m_last_visible_count++;
		} else {
			const VeMesh::AABB world_aabb = mesh_comp->getWorldAABB();
			if (isAABBInFrustum(world_aabb, planes)) {
				m_visible_game_objects[id] = VisibleObject{&obj, mesh_comp};
				m_last_visible_count++;
			}
		}
	}
}

} // namespace ve
