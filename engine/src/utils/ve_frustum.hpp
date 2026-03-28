#pragma once

#include "resources/ve_mesh.hpp"

#include <glm/glm.hpp>

namespace ve {

struct FrustumPlane {
	glm::vec4 plane; // xyz = normal, w = d (plane: dot(xyz, point) + w = 0)
};

// Gribb-Hartmann frustum plane extraction from view-projection matrix (column-major)
inline void extractFrustumPlanes(const glm::mat4& view_proj, FrustumPlane planes[6]) {
	const glm::mat4& m = view_proj;
	planes[0].plane = glm::vec4(m[0][0] + m[0][3], m[1][0] + m[1][3], m[2][0] + m[2][3], m[3][0] + m[3][3]); // left
	planes[1].plane = glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]); // right
	planes[2].plane = glm::vec4(m[0][1] + m[0][3], m[1][1] + m[1][3], m[2][1] + m[2][3], m[3][1] + m[3][3]); // bottom
	planes[3].plane = glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]); // top
	planes[4].plane = glm::vec4(m[0][2], m[1][2], m[2][2], m[3][2]); // near
	planes[5].plane = glm::vec4(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]); // far

	for (int i = 0; i < 6; ++i) {
		glm::vec3 n = glm::vec3(planes[i].plane);
		float len = glm::length(n);
		if (len > 0.0001f) {
			planes[i].plane /= len;
		}
	}
}

// Test if AABB's positive corner (relative to plane normal) is outside the plane
inline bool isAABBOutsidePlane(const VeMesh::AABB& aabb, const glm::vec4& plane) {
	const glm::vec3 n(plane.x, plane.y, plane.z);
	const float d = plane.w;
	glm::vec3 p;
	p.x = (n.x >= 0.0f) ? aabb.max.x : aabb.min.x;
	p.y = (n.y >= 0.0f) ? aabb.max.y : aabb.min.y;
	p.z = (n.z >= 0.0f) ? aabb.max.z : aabb.min.z;
	return glm::dot(n, p) + d < 0.0f;
}

// Test if AABB is inside all 6 frustum planes
inline bool isAABBInFrustum(const VeMesh::AABB& aabb, const FrustumPlane planes[6]) {
	for (int i = 0; i < 6; ++i) {
		if (isAABBOutsidePlane(aabb, planes[i].plane))
			return false;
	}
	return true;
}

} // namespace ve
