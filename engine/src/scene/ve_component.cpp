#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_mesh.hpp"
#include "ve_config.hpp"

#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>
#include <cmath>

namespace ve {

size_t ComponentTypeIDSystem::m_next_type_id = 0;

// Explicit instantiations ensure single definition across DLL boundary.
// Without these, engine and app each implicitly instantiate the template with
// different static locals so type ID mismatch and getComponent returns nullptr.
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<TransformComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<PointLightComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<DirectionalLightComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MeshComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<SpotLightComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<RigidbodyComponent>();


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
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
}

void TransformComponent::setTranslation(glm::vec3 pos) {
	translation = pos;
	m_transform_dirty = true;
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
}

void TransformComponent::setRotation(glm::quat q) {
	rotation = q;
	m_transform_dirty = true;
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
}

void TransformComponent::setScale(glm::vec3 s) {
	scale = s;
	m_transform_dirty = true;
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
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

void PointLightComponent::setIntensity(float v) {
	m_intensity = v;
	m_range_dirty = true;
}

void PointLightComponent::setColor(const glm::vec3& v) {
	m_color = v;
	m_range_dirty = true;
}

void PointLightComponent::setRange(float v) {
	m_range = v;
	m_range_dirty = true;
}

float PointLightComponent::getEffectiveRange() const {
	if (m_range_dirty)
		updateEffectiveRange();
	return m_effective_range;
}

void PointLightComponent::updateEffectiveRange() const {
	if (m_range > 0.0f) {
		m_effective_range = m_range;
	} else {
		float max_i = std::max({m_color.r * m_intensity, m_color.g * m_intensity, m_color.b * m_intensity});
		m_effective_range = std::min(std::sqrt(max_i / CLUSTER_LIGHT_CUTOFF), CLUSTER_MAX_EFFECTIVE_RANGE);
	}
	m_range_dirty = false;
}

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

// ---------------------------------------------------------------------------
// SpotLightComponent
// ---------------------------------------------------------------------------

void SpotLightComponent::setIntensity(float v) {
	m_intensity = v;
	m_range_dirty = true;
}

void SpotLightComponent::setColor(const glm::vec3& v) {
	m_color = v;
	m_range_dirty = true;
}

void SpotLightComponent::setRange(float v) {
	m_range = v;
	m_range_dirty = true;
}

void SpotLightComponent::setDirection(const glm::vec3& v) {
	m_direction = glm::normalize(v);
}

void SpotLightComponent::setInnerConeAngle(float radians) {
	m_inner_cone_angle = radians;
}

void SpotLightComponent::setOuterConeAngle(float radians) {
	m_outer_cone_angle = radians;
}

float SpotLightComponent::getEffectiveRange() const {
	if (m_range_dirty)
		updateEffectiveRange();
	return m_effective_range;
}

void SpotLightComponent::updateEffectiveRange() const {
	if (m_range > 0.0f) {
		m_effective_range = m_range;
	} else {
		float max_i = std::max({m_color.r * m_intensity, m_color.g * m_intensity, m_color.b * m_intensity});
		m_effective_range = std::min(std::sqrt(max_i / CLUSTER_LIGHT_CUTOFF), CLUSTER_MAX_EFFECTIVE_RANGE);
	}
	m_range_dirty = false;
}

} // namespace ve
