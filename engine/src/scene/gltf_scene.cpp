#include "pch.hpp"
#include "scene/gltf_scene.hpp"
#include "resources/ve_model.hpp"

namespace ve {

GltfScene::GltfScene(const SceneContext& ctx)
	: VeScene(ctx, "Empty Scene") {
	createDirectionalLight();
}

GltfScene::GltfScene(const SceneContext& ctx, const std::filesystem::path& gltf_path)
	: VeScene(ctx, "GLTF: " + gltf_path.filename().string()) {
	addModel(gltf_path);
	createDirectionalLight();
}

GltfScene::GltfScene(const SceneContext& ctx, std::unique_ptr<VeModel> model, const std::string& name)
	: VeScene(ctx, "GLTF: " + name) {
	if (model) {
		model->addToScene(m_registry, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
		m_models.push_back(std::move(model));
	}
	createDirectionalLight();
}

void GltfScene::createDirectionalLight() {
	Entity dl = m_registry.createDirectionalLight(3.0f, glm::vec3(1.0f),
		glm::normalize(glm::vec3(0.0f, -10.0f, -40.0f)));
	m_registry.setName(dl, "Directional Light");
	m_registry.getComponent<DirectionalLightComponent>(dl)->setCastsShadow(true);
}

}