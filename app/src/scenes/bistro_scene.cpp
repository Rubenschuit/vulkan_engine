#include "bistro_scene.hpp"
#include "resources/ve_model.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_game_object.hpp"
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
	if (m_game_objects.contains(m_sun_id)) {
		auto* pl = m_game_objects.at(m_sun_id).getComponent<PointLightComponent>();
		if (pl) pl->intensity = intensity;
	}
}

float BistroScene::getSunIntensity() const {
	if (m_game_objects.contains(m_sun_id)) {
		const auto* pl = m_game_objects.at(m_sun_id).getComponent<PointLightComponent>();
		return pl ? pl->intensity : DEFAULT_SUN_INTENSITY;
	}
	return DEFAULT_SUN_INTENSITY;
}

void BistroScene::loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths) {
	glm::vec3 bistro_translation = {0.0f, 0.0f, 0.0f};

	// Bistro model (root in glTF has scale 100; apply inverse scale and translation for view)
	// Use flip_tex_coord_v = true when loading mybistro/bistro.gltf
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

		glm::vec3 root_translation = glm::vec3{0.0f, 0.0f, -40.0f} + bistro_translation;
		glm::vec3 root_rotation = {0.0f, 0.0f, 0.0f};
		glm::vec3 root_scale = {4.0f, 4.0f, 4.0f};
		m_bistro_model->addToScene(m_game_objects, root_translation, root_rotation, root_scale);

		for (auto& [id, obj] : m_game_objects) {
			auto* mesh = obj.getComponent<MeshComponent>();
			if (mesh && mesh->hasMaterial() && mesh->getMaterial()->hasDescriptorSet()) {
				m_default_material_handle = mesh->getMaterialHandle();
				break;
			}
		}

		for (auto& [id, obj] : m_game_objects) {
			if (auto* mesh = obj.getComponent<MeshComponent>()) {
				mesh->has_texture = 1.0f;
			}
		}
	}

	// Main light
	{
		VeGameObject sun = VeGameObject::createPointLight(DEFAULT_SUN_INTENSITY, 4.0f, glm::vec3(1.0f, 1.0f, 1.0f));
		sun.setName("Main light");
		sun.getComponent<TransformComponent>()->setTranslation(glm::vec3{40.0f, 40.0f, 90.0f} + bistro_translation);
		sun.getComponent<PointLightComponent>()->rotates = true;
		sun.getComponent<PointLightComponent>()->casts_shadow = true;
		m_sun_id = sun.getId();
		m_game_objects.emplace(sun.getId(), std::move(sun));
	}
}

} // namespace ve
