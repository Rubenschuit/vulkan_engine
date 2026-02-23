#include "pch.hpp"
#include "scene/gltf_scene.hpp"

namespace ve {

GltfScene::GltfScene(const SceneContext& ctx)
	: VeScene(ctx, "Empty Scene"),
	  m_fallback_descriptor_set(ctx.default_material_descriptor_set) {
	createDirectionalLight();
}

GltfScene::GltfScene(const SceneContext& ctx, const std::filesystem::path& gltf_path)
	: VeScene(ctx, "GLTF: " + gltf_path.filename().string()),
	  m_fallback_descriptor_set(ctx.default_material_descriptor_set) {
	addModel(gltf_path);
	createDirectionalLight();
}

vk::raii::DescriptorSet& GltfScene::getDescriptorSet() {
	if (m_default_material_handle.isValid() && m_default_material_handle.get()->hasDescriptorSet())
		return m_default_material_handle.get()->getDescriptorSet();
	assert(m_fallback_descriptor_set && "No material descriptor set available");
	return *m_fallback_descriptor_set;
}

void GltfScene::createDirectionalLight() {
	Entity dl = m_registry.createDirectionalLight(3.0f, glm::vec3(1.0f),
		glm::normalize(glm::vec3(0.0f, -10.0f, -40.0f)));
	m_registry.setName(dl, "Directional Light");
	m_registry.getComponent<DirectionalLightComponent>(dl)->casts_shadow = true;
}

} // namespace ve