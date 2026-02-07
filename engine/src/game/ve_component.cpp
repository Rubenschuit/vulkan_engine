#include "game/ve_component.hpp"

namespace ve {

size_t ComponentTypeIDSystem::m_next_type_id = 0;

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

} // namespace ve
