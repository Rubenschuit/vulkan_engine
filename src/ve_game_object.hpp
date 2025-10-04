#pragma once

#include "ve_model.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace ve {
	class VeGameObject {
	public:
		static VeGameObject createGameObject() {
			static uint32_t current_id = 0;
			VeGameObject game_object = VeGameObject(current_id++);
			return game_object;
		}

		uint32_t getId() const { return id; }
		// Composes a transformation matrix from translation, rotation, and scale.
		glm::mat4 getTransform() const {
			const float c3 = glm::cos(rotation.z);
			const float s3 = glm::sin(rotation.z);
			const float c2 = glm::cos(rotation.x);
			const float s2 = glm::sin(rotation.x);
			const float c1 = glm::cos(rotation.y);
			const float s1 = glm::sin(rotation.y);
			return glm::mat4{
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
		}

		glm::vec3 translation{0.0f};
		glm::vec3 rotation{glm::half_pi<float>(), 0.0f, 0.0f}; // in radians
		glm::vec3 scale{1.0f};
		glm::vec3 color{1.0f};

		std::shared_ptr<VeModel> ve_model;
	private:
		VeGameObject(uint32_t id) : id(id) {}

		uint32_t id;
	};
}