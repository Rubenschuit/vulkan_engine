#pragma once

#include "resources/ve_mesh.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"

#include <glm/glm.hpp>
#include <limits>

namespace ve {

struct Ray {
	glm::vec3 origin;
	glm::vec3 direction; // must be normalized
};

struct RayHit {
	Entity entity;
	float distance = std::numeric_limits<float>::max();
	glm::vec3 point{0.0f};
};

// Slab method ray-AABB intersection.
// Returns true if ray hits the AABB. t_near is the entry distance (negative if origin is inside).
inline bool rayIntersectsAABB(const Ray& ray, const VeMesh::AABB& aabb, float& t_near) {
	glm::vec3 inv_dir = 1.0f / ray.direction;

	glm::vec3 t_min = (aabb.min - ray.origin) * inv_dir;
	glm::vec3 t_max = (aabb.max - ray.origin) * inv_dir;

	glm::vec3 t1 = glm::min(t_min, t_max);
	glm::vec3 t2 = glm::max(t_min, t_max);

	float t_entry = glm::max(glm::max(t1.x, t1.y), t1.z);
	float t_exit  = glm::min(glm::min(t2.x, t2.y), t2.z);

	t_near = t_entry;
	return t_entry <= t_exit && t_exit >= 0.0f;
}

// Generate a world-space ray from normalized viewport UV coordinates.
// uv (0,0) = top-left, (1,1) = bottom-right.
// inverse_vp = glm::inverse(camera.getProj() * camera.getView())
// where getProj() includes the Vulkan Y-flip.
inline Ray screenToWorldRay(float uv_x, float uv_y, const glm::mat4& inverse_vp) {
	float ndc_x = uv_x * 2.0f - 1.0f;
	float ndc_y = uv_y * 2.0f - 1.0f;

	glm::vec4 near_clip = glm::vec4(ndc_x, ndc_y, 0.0f, 1.0f);
	glm::vec4 far_clip  = glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);

	glm::vec4 near_world = inverse_vp * near_clip;
	glm::vec4 far_world  = inverse_vp * far_clip;

	near_world /= near_world.w;
	far_world  /= far_world.w;

	glm::vec3 origin = glm::vec3(near_world);
	glm::vec3 direction = glm::normalize(glm::vec3(far_world - near_world));

	return {origin, direction};
}

// Cast ray against all mesh entity AABBs. Returns true if hit; closest_hit is populated.
inline bool raycastScene(const Ray& ray, Registry& registry, RayHit& closest_hit) {
	float best_t = std::numeric_limits<float>::max();
	Entity best_entity = Entity::null();

	auto view = registry.view<MeshComponent, TransformComponent>();

	for (auto [entity, mesh, tc] : view) {
		if (!mesh.hasMesh())
			continue;

		const VeMesh::AABB& world_aabb = mesh.getWorldAABB();
		float t;
		if (!rayIntersectsAABB(ray, world_aabb, t))
			continue;

		float effective_t = glm::max(t, 0.0f);
		if (effective_t < best_t) {
			best_t = effective_t;
			best_entity = entity;
		}
	}

	if (best_entity.isNull())
		return false;

	closest_hit.entity = best_entity;
	closest_hit.distance = best_t;
	closest_hit.point = ray.origin + ray.direction * best_t;
	return true;
}

} // namespace ve
