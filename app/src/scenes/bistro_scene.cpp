#include "bistro_scene.hpp"
#include "resources/ve_model.hpp"
#include "scene/ve_component.hpp"
#include <glm/gtc/constants.hpp>

namespace ve {

BistroScene::BistroScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths)
	: VeScene(device, "Bistro Scene") {
	loadGameObjects(resource_manager, pool, material_layout, paths);
}

vk::raii::DescriptorSet& BistroScene::getDescriptorSet() {
	assert(m_default_material_handle.isValid() && m_default_material_handle.get()->hasDescriptorSet() && "BistroScene requires at least one textured material");
	return m_default_material_handle.get()->getDescriptorSet();
}

void BistroScene::setSunIntensity(float intensity) {
	auto* pl = m_registry.getComponent<PointLightComponent>(m_sun);
	if (pl) pl->intensity = intensity;
}

float BistroScene::getSunIntensity() const {
	const auto* pl = m_registry.getComponent<PointLightComponent>(m_sun);
	return pl ? pl->intensity : DEFAULT_SUN_INTENSITY;
}

void BistroScene::loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths) {
	glm::vec3 bistro_translation = {0.0f, 0.0f, 0.0f};

	// Bistro model
	{
		std::filesystem::path bistro_model_path = paths.bistro_model();
		m_bistro_model = VeModel::load(
			resource_manager,
			bistro_model_path.lexically_normal(),
			&pool,
			&material_layout,
			true,
			false,
			true
		);
		assert(m_bistro_model && "Failed to load Bistro model");

		glm::vec3 root_translation = glm::vec3{0.0f, 0.0f, -40.0f} + bistro_translation;
		glm::vec3 root_rotation = {0.0f, 0.0f, 0.0f};
		glm::vec3 root_scale = {1.0f, 1.0f, 1.0f};
		m_bistro_model->addToScene(m_registry, root_translation, root_rotation, root_scale);

		// Store default material for getDescriptorSet() fallback
		for (auto& mc : m_registry.meshes()) {
			if (mc.hasMaterial() && mc.getMaterial()->hasDescriptorSet()) {
				m_default_material_handle = mc.getMaterialHandle();
				break;
			}
		}
	}

	// Main light
	{
		Entity sun = m_registry.createPointLight(DEFAULT_SUN_INTENSITY, 4.0f, glm::vec3(1.0f, 1.0f, 1.0f));
		m_registry.setName(sun, "Main light");
		m_registry.getComponent<TransformComponent>(sun)->setTranslation(glm::vec3{40.0f, 40.0f, 90.0f} + bistro_translation);
		auto* pl = m_registry.getComponent<PointLightComponent>(sun);
		pl->rotates = true;
		pl->casts_shadow = true;
		m_sun = sun;
	}
}

} // namespace ve
