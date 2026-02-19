#include "scene/ve_scene.hpp"
#include "scene/ve_component.hpp"

#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>

namespace ve {

VeScene::VeScene(VeDevice& device, const std::string& name)
    : m_device(device), m_name(name), m_num_lights(0), m_num_shadow_casting_lights(0) {}

VeScene::~VeScene() = default;

void VeScene::update(float dt) {
	// Rotate point lights via Registry pool (replaces per-object PointLightComponent::update)
	auto& pl_pool = m_registry.pointLights();
	for (uint32_t i = 0; i < pl_pool.size(); i++) {
		PointLightComponent& pl = pl_pool.data()[i];
		if (!pl.getRotates())
			continue;
		uint32_t entity_idx = pl_pool.entityAt(i);
		Entity entity = m_registry.entityFromIndex(entity_idx);
		if (!m_registry.isActive(entity)) continue;
		auto* tc = m_registry.getComponent<TransformComponent>(entity);
		if (!tc)
			continue;
		const float speed = 0.04f;
		const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), speed * dt, glm::vec3(0.0f, 0.0f, 1.0f));
		glm::vec4 pos{tc->getTranslation(), 1.0f};
		pos = rot * pos;
		tc->setTranslation(glm::vec3(pos));
	}
}

} // namespace ve
