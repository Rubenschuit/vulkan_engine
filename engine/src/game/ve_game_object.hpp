/* This file defines the VeGameObject class, it requires a unique ID for each instance.
The user manually sets the object's properties (position, rotation, scale, etc.) after creation. */
#pragma once

#include "game/ve_model.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace ve {

struct TransformComponent {
	glm::vec3 translation{0.0f};
	glm::vec3 rotation{0.0f, 0.0f, 0.0f}; // in radians
	glm::vec3 scale{1.0f};
};

struct PointLightComponent {
	float intensity{1.0f};
	bool rotates{ true };
};

class VeGameObject {
public:
	static VeGameObject createGameObject() {
		static uint32_t current_id = 0;
		VeGameObject game_object = VeGameObject(current_id++);
		return game_object;
	}
	static VeGameObject createPointLight(float intensity = 1.0f, float radius = 1.0f, glm::vec3 color = glm::vec3(1.0f));

	uint32_t getId() const { return m_id; }
	// Composes a transformation matrix from translation, rotation, and scale.
	glm::mat4 getTransform() const;
	// Computes the normal matrix (inverse transpose of the model matrix).
	glm::mat3 getNormalTransform() const;

	TransformComponent transform{};
	glm::vec3 color{1.0f};
	float has_texture{0.0f};

	// optional components can be nullptr
	std::shared_ptr<VeModel> ve_model;
	std::unique_ptr<PointLightComponent> point_light_component;
private:
	VeGameObject(uint32_t id) : m_id(id) {}

	uint32_t m_id;
};
}