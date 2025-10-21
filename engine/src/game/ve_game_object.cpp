#include "game/ve_game_object.hpp"

namespace ve {

glm::mat4 VeGameObject::getTransform() const {
	const float c3 = glm::cos(transform.rotation.z);
	const float s3 = glm::sin(transform.rotation.z);
	const float c2 = glm::cos(transform.rotation.x);
	const float s2 = glm::sin(transform.rotation.x);
	const float c1 = glm::cos(transform.rotation.y);
	const float s1 = glm::sin(transform.rotation.y);
	return glm::mat4{
		{
			transform.scale.x * (c1 * c3 + s1 * s2 * s3),
			transform.scale.x * (c2 * s3),
			transform.scale.x * (c1 * s2 * s3 - c3 * s1),
			0.0f,
		},
		{
			transform.scale.y * (c3 * s1 * s2 - c1 * s3),
			transform.scale.y * (c2 * c3),
			transform.scale.y * (c1 * c3 * s2 + s1 * s3),
			0.0f,
		},
		{
			transform.scale.z * (c2 * s1),
			transform.scale.z * (-s2),
			transform.scale.z * (c1 * c2),
			0.0f,
		},
		{transform.translation.x, transform.translation.y, transform.translation.z, 1.0f}};
}

glm::mat3 VeGameObject::getNormalTransform() const {
	const float c3 = glm::cos(transform.rotation.z);
	const float s3 = glm::sin(transform.rotation.z);
	const float c2 = glm::cos(transform.rotation.x);
	const float s2 = glm::sin(transform.rotation.x);
	const float c1 = glm::cos(transform.rotation.y);
	const float s1 = glm::sin(transform.rotation.y);
	const glm::vec3 inverse_scale = 1.0f / transform.scale;
	return glm::mat3{
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
}

VeGameObject VeGameObject::createPointLight(float intensity, float radius, glm::vec3 color) {
	VeGameObject game_object = VeGameObject::createGameObject();
	game_object.point_light_component = std::make_unique<PointLightComponent>();
	game_object.point_light_component->intensity = intensity;
	game_object.color = color;
	game_object.has_texture = 0.0f;
	game_object.transform.scale = glm::vec3(radius); // uniform scale for point light quad size
	return game_object;
}

}