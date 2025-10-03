#pragma once

#include "ve_model.hpp"

namespace ve {
	class VeGameObject {
	public:
		static VeGameObject createGameObject() {
			static uint32_t current_id = 0;
			VeGameObject game_object = VeGameObject(current_id++);
			return game_object;
		}

		uint32_t getId() const { return id; }

		glm::vec3 position{0.0f};
		glm::vec3 rotation{0.0f};
		glm::vec3 scale{1.0f};
		glm::vec3 color{1.0f};

		std::shared_ptr<VeModel> ve_model;
	private:
		VeGameObject(uint32_t id) : id(id) {}

		uint32_t id;
	};
}