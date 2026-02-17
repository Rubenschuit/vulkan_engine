#include "gltf_scene.hpp"
#include "resources/ve_model.hpp"
#include "scene/ve_component.hpp"

namespace ve {

GltfScene::GltfScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout,
					 vk::raii::DescriptorSet* fallback_descriptor_set)
	: VeScene(device, "Empty Scene"),
	  m_resource_manager(resource_manager), m_pool(pool), m_material_layout(material_layout),
	  m_fallback_descriptor_set(fallback_descriptor_set) {
	createSunLight();
}

GltfScene::GltfScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout,
					 const std::filesystem::path& gltf_path, vk::raii::DescriptorSet* fallback_descriptor_set)
	: VeScene(device, "GLTF: " + gltf_path.filename().string()),
	  m_resource_manager(resource_manager), m_pool(pool), m_material_layout(material_layout),
	  m_fallback_descriptor_set(fallback_descriptor_set) {
	addModel(gltf_path);
	createSunLight();
}

void GltfScene::addModel(const std::filesystem::path& gltf_path) {

	auto model = VeModel::load(m_resource_manager, gltf_path.lexically_normal(), &m_pool, &m_material_layout,
		/*extract_lights=*/true, /*extract_fixture_lights=*/false, /*flip_tex_coord_v=*/true);

	if (model) {
		model->addToScene(m_registry, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});

		if (!m_default_material_handle.isValid()) {
			for (auto& mc : m_registry.meshes()) {
				if (mc.hasMaterial() && mc.getMaterial()->hasDescriptorSet()) {
					m_default_material_handle = mc.getMaterialHandle();
					break;
				}
			}
		}

		m_models.push_back(std::move(model));
	} else {
		VE_LOGE("Failed to load GLTF model: " << gltf_path);
	}
}

vk::raii::DescriptorSet& GltfScene::getDescriptorSet() {
	if (m_default_material_handle.isValid() && m_default_material_handle.get()->hasDescriptorSet()) {
		return m_default_material_handle.get()->getDescriptorSet();
	}
	assert(m_fallback_descriptor_set && "No material descriptor set available");
	return *m_fallback_descriptor_set;
}

void GltfScene::setSunIntensity(float intensity) {
	auto* pl = m_registry.getComponent<PointLightComponent>(m_sun);
	if (pl) pl->intensity = intensity;
}

float GltfScene::getSunIntensity() const {
	const auto* pl = m_registry.getComponent<PointLightComponent>(m_sun);
	return pl ? pl->intensity : DEFAULT_SUN_INTENSITY;
}

void GltfScene::createSunLight() {
	m_sun = m_registry.createPointLight(DEFAULT_SUN_INTENSITY, 4.0f, glm::vec3(1.0f, 1.0f, 1.0f));
	m_registry.setName(m_sun, "Sun");
	m_registry.getComponent<TransformComponent>(m_sun)->setTranslation({0.0f, 10.0f, 40.0f});
	auto* sun_pl = m_registry.getComponent<PointLightComponent>(m_sun);
	sun_pl->rotates = true;
	sun_pl->casts_shadow = true;
}

} // namespace ve
