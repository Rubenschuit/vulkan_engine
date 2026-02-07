#include "game/ve_game_object.hpp"
#include "game/ve_component.hpp"
#include <atomic>

namespace ve {

// Thread-safe and DLL-safe ID generation
static std::atomic<uint32_t> current_id{0};

VeGameObject VeGameObject::createGameObject() {
	VeGameObject game_object{current_id.fetch_add(1)};
	game_object.addComponent<TransformComponent>();
	return game_object;
}

VeGameObject VeGameObject::createPointLight(float intensity, float radius, glm::vec3 color) {
	VeGameObject game_object = createGameObject();
	auto* pl = game_object.addComponent<PointLightComponent>();
	pl->intensity = intensity;
	pl->rotates = true;
	pl->casts_shadow = true;

	auto* mat = game_object.addComponent<MaterialComponent>();
	mat->color = color;
	mat->has_texture = 0.0f;
	mat->has_shadow = true;

	auto* transform = game_object.getComponent<TransformComponent>();
	transform->scale = glm::vec3(radius);
	return game_object;
}

const glm::mat4& VeGameObject::getTransform() const {
	auto* transform = getComponent<TransformComponent>();
	assert(transform && "VeGameObject must have TransformComponent");
	return transform->getTransform();
}

const glm::mat3& VeGameObject::getNormalTransform() const {
	auto* transform = getComponent<TransformComponent>();
	assert(transform && "VeGameObject must have TransformComponent");
	return transform->getNormalTransform();
}

} // namespace ve
