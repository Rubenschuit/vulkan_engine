#include "bistro_scene.hpp"

namespace ve {

BistroScene::BistroScene(const SceneContext& ctx, const AssetPaths& paths)
	: VeScene(ctx, "Bistro Scene") {

	// Directional light
	Entity dl = m_registry.createDirectionalLight(5.0f, glm::vec3(1.0f),
		glm::normalize(glm::vec3(-1.0f, -0.5f, -2.5f)));
	m_registry.setName(dl, "Directional Light");
	m_registry.getComponent<DirectionalLightComponent>(dl)->setCastsShadow(true);

	placeModel({
		.gltf_path = paths.bistro_model().lexically_normal(),
		.scale = {2.0f, 2.0f, 2.0f},
		.flip_tex_coord_v = true,
		.on_loaded = [this](Entity wrapper) {
			// Example callback
			m_registry.setName(wrapper, "Bistro Model");
		}
	});
}

}
