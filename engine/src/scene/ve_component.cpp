#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_mesh.hpp"

#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace ve {

size_t ComponentTypeIDSystem::m_next_type_id = 0;

// Explicit instantiations ensure single definition across DLL boundary.
// Without these, engine and app each implicitly instantiate the template with
// different static locals so type ID mismatch and getComponent returns nullptr.
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<TransformComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<PointLightComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MeshComponent>();


// ---------------------------------------------------------------------------
// TransformComponent
// ---------------------------------------------------------------------------

const glm::mat4& TransformComponent::getTransform() const {
	if (m_transform_dirty)
		updateTransform();
	return m_cached_transform;
}

const glm::mat3& TransformComponent::getNormalTransform() const {
	if (m_transform_dirty)
		updateTransform();
	return m_cached_normal_transform;
}

void TransformComponent::setRotationEuler(glm::vec3 euler_rad) {
	// Match previous Euler order: R = Rz(z) * Ry(y) * Rx(x)
	rotation = glm::quat_cast(glm::eulerAngleZYX(euler_rad.z, euler_rad.y, euler_rad.x));
	m_transform_dirty = true;
}

void TransformComponent::setTranslation(glm::vec3 pos) {
	translation = pos;
	m_transform_dirty = true;
}

void TransformComponent::setRotation(glm::quat q) {
	rotation = q;
	m_transform_dirty = true;
}

void TransformComponent::setScale(glm::vec3 s) {
	scale = s;
	m_transform_dirty = true;
}

// Updates mutable private members m_cached_transform, m_cached_normal_transform
// and m_transform_dirty.
void TransformComponent::updateTransform() const {
	const glm::mat4 R = glm::mat4_cast(rotation);
	const glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
	const glm::mat4 T = glm::translate(glm::mat4(1.0f), translation);
	m_cached_transform = T * R * S;

	const glm::vec3 inverse_scale = 1.0f / scale;
	const glm::mat3 R3 = glm::mat3_cast(rotation);
	const glm::mat3 S3(inverse_scale.x, 0.0f, 0.0f, 0.0f, inverse_scale.y, 0.0f, 0.0f, 0.0f, inverse_scale.z);
	m_cached_normal_transform = R3 * S3;

	m_transform_dirty = false;
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
}


// ---------------------------------------------------------------------------
// PointLightComponent
// ---------------------------------------------------------------------------

void PointLightComponent::update(float /*delta_time*/) {}

// ---------------------------------------------------------------------------
// MeshComponent
// ---------------------------------------------------------------------------

const VeMesh::AABB& MeshComponent::getWorldAABB() const {
	if (m_world_aabb_dirty)
		updateWorldAABB();
	return m_cached_world_aabb;
}

void MeshComponent::invalidateWorldAABB() {
	m_world_aabb_dirty = true;
}

void MeshComponent::updateWorldAABB() const {
	assert(m_registry && "MeshComponent must have Registry context for world AABB");
	const glm::mat4& model = m_registry->getWorldTransform(m_entity);
	m_cached_world_aabb = transformAABB(getMesh()->getLocalAABB(), model);
	m_world_aabb_dirty = false;
}

void MeshComponent::render() {
// unused, render systems handle this for now
}

} // namespace ve
