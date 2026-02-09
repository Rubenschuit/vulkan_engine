#include "scene/ve_component.hpp"
#include "scene/ve_game_object.hpp"
#include "resources/ve_mesh.hpp"

#include <cstring>

#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>

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
	updateMatrices();
	return m_cached_transform;
}

const glm::mat3& TransformComponent::getNormalTransform() const {
	updateMatrices();
	return m_cached_normal_transform;
}

void TransformComponent::updateMatrices() const {
	if (translation == m_last_translation &&
	    rotation == m_last_rotation &&
	    scale == m_last_scale) {
		return;
	}

	const float c3 = glm::cos(rotation.z);
	const float s3 = glm::sin(rotation.z);
	const float c2 = glm::cos(rotation.x);
	const float s2 = glm::sin(rotation.x);
	const float c1 = glm::cos(rotation.y);
	const float s1 = glm::sin(rotation.y);

	m_cached_transform = glm::mat4{
	    {
	        scale.x * (c1 * c3 + s1 * s2 * s3),
	        scale.x * (c2 * s3),
	        scale.x * (c1 * s2 * s3 - c3 * s1),
	        0.0f,
	    },
	    {
	        scale.y * (c3 * s1 * s2 - c1 * s3),
	        scale.y * (c2 * c3),
	        scale.y * (c1 * c3 * s2 + s1 * s3),
	        0.0f,
	    },
	    {
	        scale.z * (c2 * s1),
	        scale.z * (-s2),
	        scale.z * (c1 * c2),
	        0.0f,
	    },
	    {translation.x, translation.y, translation.z, 1.0f}};

	const glm::vec3 inverse_scale = 1.0f / scale;
	m_cached_normal_transform = glm::mat3{
	    {
	        inverse_scale.x * (c1 * c3 + s1 * s2 * s3),
	        inverse_scale.x * (c2 * s3),
	        inverse_scale.x * (c1 * s2 * s3 - c3 * s1)
	    },
	    {
	        inverse_scale.y * (c3 * s1 * s2 - c1 * s3),
	        inverse_scale.y * (c2 * c3),
	        inverse_scale.y * (c1 * c3 * s2 + s1 * s3)
	    },
	    {
	        inverse_scale.z * (c2 * s1),
	        inverse_scale.z * (-s2),
	        inverse_scale.z * (c1 * c2)
	    }
	};

	m_last_translation = translation;
	m_last_rotation = rotation;
	m_last_scale = scale;
}

// ---------------------------------------------------------------------------
// PointLightComponent
// ---------------------------------------------------------------------------

void PointLightComponent::update(float delta_time) {
	if (!rotates)
		return;

	auto* transform = m_owner->getComponent<TransformComponent>();
	if (!transform)
		return;

	const float speed = 0.04f;
	const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), speed * delta_time, glm::vec3(0.0f, 0.0f, 1.0f));
	glm::vec4 pos{transform->translation, 1.0f};
	pos = rot * pos;
	transform->translation = glm::vec3(pos);
}

// ---------------------------------------------------------------------------
// MeshComponent
// ---------------------------------------------------------------------------

VeMesh::AABB MeshComponent::getWorldAABB() const {
	const glm::mat4& model = getOwner()->getTransform();
	const bool transform_changed = std::memcmp(&m_last_model_matrix[0], &model[0], sizeof(glm::mat4)) != 0;
	if (!m_world_aabb_valid || transform_changed) {
		m_cached_world_aabb = transformAABB(getMesh()->getLocalAABB(), model);
		m_last_model_matrix = model;
		m_world_aabb_valid = true;
	}
	return m_cached_world_aabb;
}

void MeshComponent::render() {
// unused
}

} // namespace ve
