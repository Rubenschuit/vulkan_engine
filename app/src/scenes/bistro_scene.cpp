#include "bistro_scene.hpp"
#include <glm/gtc/constants.hpp>

namespace ve {

BistroScene::BistroScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths)
	: VeScene(device, resource_manager, pool, material_layout, "Bistro Scene") {
	loadGameObjects(paths);
}

vk::raii::DescriptorSet& BistroScene::getDescriptorSet() {
	assert(m_default_material_handle.isValid() && m_default_material_handle.get()->hasDescriptorSet() && "BistroScene requires at least one textured material");
	return m_default_material_handle.get()->getDescriptorSet();
}

void BistroScene::loadGameObjects(const AssetPaths& paths) {
	glm::vec3 bistro_translation = {0.0f, 0.0f, 0.0f};

	// Bistro model
	{
		std::filesystem::path bistro_model_path = paths.bistro_model();
		m_bistro_model = VeModel::load(
			m_resource_manager,
			bistro_model_path.lexically_normal(),
			&m_pool,
			&m_material_layout,
			true,
			true
		);
		assert(m_bistro_model && "Failed to load Bistro model");

		glm::vec3 root_translation = glm::vec3{0.0f, 0.0f, 0.0f} + bistro_translation;
		glm::vec3 root_rotation = {0.0f, 0.0f, 0.0f};
		glm::vec3 root_scale = {2.0f, 2.0f, 2.0f};
		m_bistro_model->addToScene(m_registry, root_translation, root_rotation, root_scale);

		// Store default material for getDescriptorSet() fallback
		for (auto& mc : m_registry.meshes()) {
			if (mc.hasMaterial() && mc.getMaterial()->hasDescriptorSet()) {
				m_default_material_handle = mc.getMaterialHandle();
				break;
			}
		}
	}

	// Directional light
	{
		Entity dl = m_registry.createDirectionalLight(5.0f, glm::vec3(1.0f),
			glm::normalize(glm::vec3(-1.0f, -0.5f, -2.5f)));
		m_registry.setName(dl, "Directional Light");
		m_registry.getComponent<DirectionalLightComponent>(dl)->casts_shadow = true;
	}
}

} // namespace ve
