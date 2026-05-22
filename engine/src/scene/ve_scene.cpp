#include "scene/ve_scene.hpp"
#include "scene/ve_component.hpp"
#include "scene/scene_manager.hpp"

#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>

namespace ve {

VeScene::VeScene(const SceneContext& ctx, const std::string& name)
    : m_device(ctx.device), m_resource_manager(ctx.resource_manager),
      m_event_bus(ctx.event_bus),
      m_name(name), m_num_lights(0), m_num_shadow_casting_lights(0) {}

VeScene::~VeScene() = default;

void VeScene::placeModel(const AddModelRequestedEvent& request) {
	m_event_bus.emitImmediate(request);
}

void VeScene::update(float dt) {
	for (auto& animator : m_registry.animators())
		animator.update(dt);

	for (auto [entity, pl, tc] : m_registry.view<PointLightComponent, TransformComponent>()) {
		if (!pl.getRotates())
			continue;
		const float speed = 0.04f;
		const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), speed * dt, glm::vec3(0.0f, 0.0f, 1.0f));
		glm::vec4 pos{tc.getTranslation(), 1.0f};
		pos = rot * pos;
		tc.setTranslation(glm::vec3(pos));
	}
}

} // namespace ve
