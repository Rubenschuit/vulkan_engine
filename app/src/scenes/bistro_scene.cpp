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
	auto* dl = m_registry.getComponent<DirectionalLightComponent>(m_sun);
	if (dl)
		dl->intensity = intensity;
}

float BistroScene::getSunIntensity() const {
	const auto* dl = m_registry.getComponent<DirectionalLightComponent>(m_sun);
	return dl ? dl->intensity : DEFAULT_SUN_INTENSITY;
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

	// Main light (directional)
	{
		Entity sun = m_registry.createDirectionalLight(DEFAULT_SUN_INTENSITY, glm::vec3(1.0f),
			glm::normalize(glm::vec3(-1.0f, -0.5f, -2.5f)));
		m_registry.setName(sun, "Main light");
		m_registry.getComponent<DirectionalLightComponent>(sun)->casts_shadow = true;
		m_sun = sun;
	}
}

} // namespace ve
