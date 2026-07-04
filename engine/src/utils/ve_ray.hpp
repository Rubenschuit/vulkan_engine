#pragma once

#include "resources/ve_mesh.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cassert>
#include <limits>
#include <vector>

namespace ve {

struct Ray {
	glm::vec3 origin;
	glm::vec3 direction; // must be normalized
};

struct RayHit {
	Entity entity;
	float distance = std::numeric_limits<float>::max();
	glm::vec3 point{0.0f};
	glm::vec3 normal{0.0f};
};

// Slab method ray-AABB intersection.
// Returns true if ray hits the AABB.
// t_near is the entry distance (negative if origin is inside).
inline bool rayIntersectsAABB(const Ray& ray, const VeMesh::AABB& aabb, float& t_near) {
	glm::vec3 reciprocal_dir = 1.0f / ray.direction;

	glm::vec3 t_min = (aabb.min - ray.origin) * reciprocal_dir;
	glm::vec3 t_max = (aabb.max - ray.origin) * reciprocal_dir;

	glm::vec3 t1 = glm::min(t_min, t_max);
	glm::vec3 t2 = glm::max(t_min, t_max);

	float t_entry = glm::max(glm::max(t1.x, t1.y), t1.z);
	float t_exit  = glm::min(glm::min(t2.x, t2.y), t2.z);

	t_near = t_entry;
	return t_entry <= t_exit && t_exit >= 0.0f;
}

// Möller-Trumbore ray-triangle intersection (both-sided).
// Returns true on hit, sets t (distance along ray) and u/v (barycentric coords).
inline bool rayIntersectsTriangle(const Ray& ray,
                                  const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                  float& t, float& u, float& v) {
	constexpr float EPSILON = 1e-8f;
	glm::vec3 e1 = v1 - v0;
	glm::vec3 e2 = v2 - v0;
	glm::vec3 h = glm::cross(ray.direction, e2);
	float det = glm::dot(e1, h);

	if (det > -EPSILON && det < EPSILON)
		return false;

	float inv_det = 1.0f / det;
	glm::vec3 s = ray.origin - v0;
	u = glm::dot(s, h) * inv_det;
	if (u < 0.0f || u > 1.0f)
		return false;

	glm::vec3 q = glm::cross(s, e1);
	v = glm::dot(ray.direction, q) * inv_det;
	if (v < 0.0f || u + v > 1.0f)
		return false;

	t = glm::dot(e2, q) * inv_det;
	return t > EPSILON;
}

// Test ray against a mesh's triangles in local space.
// The ray is transformed into local space to avoid per-triangle world transforms.
// Returns true on hit. out_world_t is world-space distance, out_normal is world-space face normal.
inline bool raycastMesh(const Ray& ray, const VeMesh& mesh,
                        const glm::mat4& model, const glm::mat4& inv_model,
                        float& out_world_t, glm::vec3& out_normal) {
	if (!mesh.hasCpuGeometry())
		return false;

	const auto& positions = mesh.getCpuPositions();
	const auto& indices = mesh.getCpuIndices();

	// Transform ray into mesh-local space
	glm::vec3 local_origin = glm::vec3(inv_model * glm::vec4(ray.origin, 1.0f));
	glm::vec3 local_dir = glm::vec3(inv_model * glm::vec4(ray.direction, 0.0f));
	float dir_len = glm::length(local_dir);
	if (dir_len < 1e-10f)
		return false;
	local_dir /= dir_len;

	Ray local_ray{
		.origin = local_origin,
		.direction = local_dir
	};

	float best_local_t = std::numeric_limits<float>::max();
	glm::vec3 best_local_normal{0.0f};
	bool hit = false;

	assert(indices.size() % 3 == 0 && "Index count must be a multiple of 3");
	uint32_t tri_count = static_cast<uint32_t>(indices.size()) / 3;
	for (uint32_t i = 0; i < tri_count; i++) {
		assert(indices[i * 3 + 0] < positions.size() && indices[i * 3 + 1] < positions.size() &&
		       indices[i * 3 + 2] < positions.size() && "Mesh index out of range");
		const glm::vec3& v0 = positions[indices[i * 3 + 0]];
		const glm::vec3& v1 = positions[indices[i * 3 + 1]];
		const glm::vec3& v2 = positions[indices[i * 3 + 2]];

		float t, u, v;
		if (rayIntersectsTriangle(local_ray, v0, v1, v2, t, u, v) && t < best_local_t) {
			glm::vec3 face_normal = glm::cross(v1 - v0, v2 - v0);
			if (glm::dot(face_normal, face_normal) < 1e-16f)
				continue; // skip degenerate triangle
			best_local_t = t;
			best_local_normal = face_normal;
			hit = true;
		}
	}

	if (!hit)
		return false;

	// Convert local-space hit to world-space distance
	glm::vec3 local_hit = local_origin + local_dir * best_local_t;
	glm::vec3 world_hit = glm::vec3(model * glm::vec4(local_hit, 1.0f));
	out_world_t = glm::length(world_hit - ray.origin);

	// Transform normal to world space
	glm::mat3 normal_matrix = glm::transpose(glm::mat3(inv_model));
	out_normal = glm::normalize(normal_matrix * best_local_normal);

	return true;
}

// Generate a world-space ray from normalized viewport UV coordinates.
// uv (0,0) is top-left, (1,1) is bottom-right.
// Reverse-Z: near plane = NDC z=1, far plane = NDC z=0 (infinity).
inline Ray screenToWorldRay(float uv_x, float uv_y, const glm::mat4& inverse_vp) {
	float ndc_x = uv_x * 2.0f - 1.0f;
	float ndc_y = uv_y * 2.0f - 1.0f;

	glm::vec4 near_clip = glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);
	glm::vec4 far_clip  = glm::vec4(ndc_x, ndc_y, 1e-4f, 1.0f);

	glm::vec4 near_world = inverse_vp * near_clip;
	glm::vec4 far_world  = inverse_vp * far_clip;

	near_world /= near_world.w;
	far_world  /= far_world.w;

	glm::vec3 origin = glm::vec3(near_world);
	glm::vec3 direction = glm::normalize(glm::vec3(far_world - near_world));

	return {origin, direction};
}

// Ray-sphere intersection. Returns true if ray hits the sphere, sets t_hit to nearest positive intersection.
// solves quadratic equation for ray-sphere intersection.
inline bool rayIntersectsSphere(const Ray& ray, const glm::vec3& center, float radius, float& t_hit) {
	glm::vec3 oc = ray.origin - center;
	float b = glm::dot(oc, ray.direction);
	float c = glm::dot(oc, oc) - radius * radius;
	float discriminant = b * b - c;
	if (discriminant < 0.0f)
		return false;
	float sqrt_d = std::sqrt(discriminant);
	float t0 = -b - sqrt_d;
	float t1 = -b + sqrt_d;
	if (t1 < 0.0f)
		return false;
	t_hit = (t0 >= 0.0f) ? t0 : t1;
	return true;
}

// Cast ray against scene: AABB broad phase into triangle narrow phase with early-out.

// Accepts a hit when it lands inside the parallelogram spanned by right_half
// and up_half, centered at center. The vectors are not required to be
// orthogonal or normalized.
inline bool rayIntersectsQuad(const Ray& ray, const glm::vec3& center,
                              const glm::vec3& right_half, const glm::vec3& up_half, float& t_out) {
	glm::vec3 n = glm::cross(up_half, right_half);
	float denom = glm::dot(ray.direction, n);
	if (std::abs(denom) < 1e-8f)
		return false;
	float t = glm::dot(center - ray.origin, n) / denom;
	if (t < 0.0f)
		return false;
	glm::vec3 d = ray.origin + ray.direction * t - center;
	// Solve d = a*right_half + b*up_half. Accept |a|,|b| <= 1
	float rr = glm::dot(right_half, right_half);
	float uu = glm::dot(up_half, up_half);
	float ru = glm::dot(right_half, up_half);
	float det = rr * uu - ru * ru;
	if (det < 1e-12f)
		return false;
	float dr = glm::dot(d, right_half);
	float du = glm::dot(d, up_half);
	float a = (dr * uu - du * ru) / det;
	float b = (du * rr - dr * ru) / det;
	if (std::abs(a) > 1.0f || std::abs(b) > 1.0f)
		return false;
	t_out = t;
	return true;
}

// Also tests light entities against spheres for viewport picking.
inline bool raycastScene(const Ray& ray, Registry& registry, RayHit& closest_hit) {
	struct AabbCandidate {
		Entity entity;
		MeshComponent* mesh;
		glm::mat4 model;
		glm::mat4 inv_model;
		float aabb_t;
	};

	// Broad phase: collect all AABB hits
	std::vector<AabbCandidate> candidates;
	auto view = registry.view<MeshComponent, TransformComponent>();

	for (auto [entity, mesh, tc] : view) {
		if (!mesh.hasMesh())
			continue;

		const VeMesh::AABB& world_aabb = mesh.getWorldAABB();
		float t;
		if (!rayIntersectsAABB(ray, world_aabb, t))
			continue;

		const glm::mat4& model = registry.getWorldTransform(entity);
		candidates.push_back({entity, &mesh, model, glm::inverse(model), glm::max(t, 0.0f)});
	}

	float best_t = std::numeric_limits<float>::max();
	Entity best_entity = Entity::null();
	glm::vec3 best_normal{0.0f};

	if (!candidates.empty()) {
		// Sort by AABB distance
		std::sort(candidates.begin(), candidates.end(),
				[](const AabbCandidate& a, const AabbCandidate& b) { return a.aabb_t < b.aabb_t; });

		// Narrow phase: test triangles per candidate, early-out when no closer hit possible
		for (const auto& c : candidates) {
			if (c.aabb_t >= best_t)
				break; // no candidate can beat current best because AABBs are sorted by distance

			float tri_t;
			glm::vec3 tri_normal;
			if (raycastMesh(ray, *c.mesh->getMesh(), c.model, c.inv_model, tri_t, tri_normal) && tri_t < best_t) {
				best_t = tri_t;
				best_entity = c.entity;
				best_normal = tri_normal;
			}
		}
	}

	// Test point lights and spot lights as spheres
	constexpr float LIGHT_PICK_RADIUS = 0.35f;

	auto testLightEntity = [&](Entity entity) {
		auto* tc = registry.getComponent<TransformComponent>(entity);
		if (!tc)
			return;
		glm::vec3 pos = registry.getWorldTransform(entity)[3];
		float t;
		if (rayIntersectsSphere(ray, pos, LIGHT_PICK_RADIUS, t) && t < best_t) {
			best_t = t;
			best_entity = entity;
			best_normal = glm::normalize(ray.origin + ray.direction * t - pos);
		}
	};

	for (auto [entity, pl, tc] : registry.view<PointLightComponent, TransformComponent>())
		testLightEntity(entity);
	for (auto [entity, sl, tc] : registry.view<SpotLightComponent, TransformComponent>())
		testLightEntity(entity);

	for (auto [entity, al, tc] : registry.view<AreaLightComponent, TransformComponent>()) {
		// Quad picking only while the gizmo is shown
		if (al.getShowGizmo()) {
			const glm::mat4& world = registry.getWorldTransform(entity);
			AreaLightBasis basis = areaLightWorldBasis(world);
			glm::vec3 center = glm::vec3(world[3]);
			glm::vec3 n_raw = glm::cross(basis.up_half, basis.right_half);
			float t;
			if (glm::length(n_raw) > 1e-12f
				&& rayIntersectsQuad(ray, center, basis.right_half, basis.up_half, t)
				&& t < best_t) {
				best_t = t;
				best_entity = entity;
				glm::vec3 n = glm::normalize(n_raw);
				best_normal = (glm::dot(n, ray.direction) > 0.0f) ? -n : n;
				continue;
			}
		}
		testLightEntity(entity);
	}

	if (best_entity.isNull())
		return false;

	closest_hit.entity = best_entity;
	closest_hit.distance = best_t;
	closest_hit.point = ray.origin + ray.direction * best_t;
	closest_hit.normal = best_normal;
	return true;
}

} // namespace ve
