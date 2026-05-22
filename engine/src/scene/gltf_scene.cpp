#include "pch.hpp"
#include "scene/gltf_scene.hpp"
#include "scene/ve_component.hpp"

namespace ve {

GltfScene::GltfScene(const SceneContext& ctx)
	: VeScene(ctx, "Empty Scene") {
	createDirectionalLight();
}

void GltfScene::createDirectionalLight() {
	Entity dl = m_registry.createDirectionalLight(3.0f, glm::vec3(1.0f),
		glm::normalize(glm::vec3(0.0f, -10.0f, -40.0f)));
	m_registry.setName(dl, "Directional Light");
	m_registry.getComponent<DirectionalLightComponent>(dl)->setCastsShadow(true);
}

}