#include "gltf_scene.hpp"

namespace ve {

GltfScene::GltfScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout,
					 vk::raii::DescriptorSet* fallback_descriptor_set)
	: VeScene(device, resource_manager, pool, material_layout, "Empty Scene"),
	  m_fallback_descriptor_set(fallback_descriptor_set) {
	createDirectionalLight();
}

GltfScene::GltfScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout,
					 const std::filesystem::path& gltf_path, vk::raii::DescriptorSet* fallback_descriptor_set)
	: VeScene(device, resource_manager, pool, material_layout, "GLTF: " + gltf_path.filename().string()),
	  m_fallback_descriptor_set(fallback_descriptor_set) {
	addModel(gltf_path);
	createDirectionalLight();
}

vk::raii::DescriptorSet& GltfScene::getDescriptorSet() {
	if (m_default_material_handle.isValid() && m_default_material_handle.get()->hasDescriptorSet()) {
		return m_default_material_handle.get()->getDescriptorSet();
	}
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
