#include "scene/ve_game_object.hpp"
#include "scene/ve_component.hpp"
#include <algorithm>
#include <atomic>

namespace ve {

// Thread-safe and DLL-safe ID generation
static std::atomic<uint32_t> current_id{0};

VeGameObject VeGameObject::createGameObject() {
	VeGameObject game_object{current_id.fetch_add(1)};
	game_object.addComponent<TransformComponent>();
	return game_object;
}

VeGameObject VeGameObject::createGameObject(const std::string& name) {
	VeGameObject game_object{current_id.fetch_add(1)};
	game_object.m_name = std::move(name);
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
	const glm::mat4& local = transform->getTransform();
	if (m_parent) {
		m_cached_world_transform = m_parent->getTransform() * local;
		return m_cached_world_transform;
	}
	return local;
}

const glm::mat3& VeGameObject::getNormalTransform() const {
	auto* transform = getComponent<TransformComponent>();
	assert(transform && "VeGameObject must have TransformComponent");
	if (m_parent) {
		// World normal = inverse(transpose(mat3(world_transform)))
		const glm::mat4& world = getTransform();
		m_cached_world_normal = glm::mat3(glm::inverse(glm::transpose(world)));
		return m_cached_world_normal;
	}
	return transform->getNormalTransform();
}

void VeGameObject::setParent(VeGameObject* parent) {
	if (m_parent == parent)
		return;
	if (m_parent) {
		auto& siblings = m_parent->m_children;
		siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());
	}
	m_parent = parent;
	if (m_parent) {
		m_parent->m_children.push_back(this);
	}
}

void VeGameObject::addChild(VeGameObject* child) {
	child->setParent(this);
}

} // namespace ve
