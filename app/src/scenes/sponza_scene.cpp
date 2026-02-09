#include "sponza_scene.hpp"
#include "game/ve_model.hpp"
#include "game/ve_component.hpp"
#include "game/ve_game_object.hpp"

namespace ve {

SponzaScene::SponzaScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, const char* variant)
    : VeScene(device, "Sponza Scene") {
    loadGameObjects(resource_manager, pool, material_layout, paths, variant);
}

vk::raii::DescriptorSet& SponzaScene::getDescriptorSet() {
    return m_sponza_model->getMaterialDescriptorSet(0);
}

vk::raii::DescriptorSet& SponzaScene::getDescriptorSet(const VeGameObject* obj) {
    if (!obj) {// return default descriptor set if no object is provided
		VE_LOGW("No object provided to getDescriptorSet");
		return getDescriptorSet();
    }
    const auto* mesh = obj->getComponent<MeshComponent>();
    if (!mesh || !mesh->hasMesh()) {
		VE_LOGW("Mesh not found in object");
		return getDescriptorSet(); // return default descriptor set if mesh is not found
	}
    uint32_t mat_idx = mesh->getMaterialIndex();
    if (mat_idx >= m_sponza_model->getMaterialCount()) {
		VE_LOGW("Material index out of range");
		return getDescriptorSet(); // return default descriptor set if material index is out of range
	}
    return m_sponza_model->getMaterialDescriptorSet(mat_idx);
}

MaterialAlphaProps SponzaScene::getMaterialAlphaProps(const VeGameObject* obj) const {
    if (!obj) {// return default alpha props if no object is provided
		VE_LOGW("No object provided to getMaterialAlphaProps");
		return {};
	}
    const auto* mesh = obj->getComponent<MeshComponent>();
    if (!mesh || !mesh->hasMesh()) {
		VE_LOGW("Mesh not found in object");
		return {};
	}
    return m_sponza_model->getMaterialAlphaProps(mesh->getMaterialIndex());
}

void SponzaScene::setSunIntensity(float intensity) {
    if (m_game_objects.contains(m_sun_id)) {
        auto* pl = m_game_objects.at(m_sun_id).getComponent<PointLightComponent>();
        if (pl) pl->intensity = intensity;
    }
}

void SponzaScene::loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, const char* variant) {
    glm::vec3 sponza_translation = {0.0f, 0.0f, 300.0f};

    // Sponza model (variant: sponza, sponza_low, sponza_high)
    {
        std::filesystem::path sponza_model_path = paths.sponza_model(variant);
        m_sponza_model = VeModel::load(m_device, resource_manager, sponza_model_path.lexically_normal(), &pool, &material_layout);
        assert(m_sponza_model && "Failed to load Sponza model");

        glm::vec3 root_translation = glm::vec3{0.0f, 0.0f, -350.0f} + sponza_translation;
        glm::vec3 root_rotation = {0.0f, 0.0f, 0.0f};
        glm::vec3 root_scale = {12.5f, 12.5f, 12.5f};
        m_sponza_model->addToScene(m_game_objects, root_translation, root_rotation, root_scale);

        // Add MaterialComponent to all mesh objects for textured materials
        for (auto& [id, obj] : m_game_objects) {
            if (obj.getComponent<MeshComponent>()) {
                auto* mat = obj.addComponent<MaterialComponent>();
                mat->has_texture = 1.0f;
            }
        }
    }

    // sponza sun light
    {
        VeGameObject sun = VeGameObject::createPointLight(2000.0f, 4.0f, glm::vec3(1.0f, 1.0f, 1.0f));
        sun.getComponent<TransformComponent>()->translation = glm::vec3{0.0f, 50.0f, -140.0f} + sponza_translation;
        sun.getComponent<PointLightComponent>()->rotates = true;
        m_sun_id = sun.getId();
        m_game_objects.emplace(sun.getId(), std::move(sun));
    }

    // sponza fire lights
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.getComponent<TransformComponent>()->translation = glm::vec3{-62.0f, -22.0f, -336.0f} + sponza_translation;
        fire.getComponent<PointLightComponent>()->rotates = false;
        fire.getComponent<PointLightComponent>()->casts_shadow = false;
        fire.getComponent<MaterialComponent>()->has_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.getComponent<TransformComponent>()->translation = glm::vec3{-62.0f, 14.0f, -336.0f} + sponza_translation;
        fire.getComponent<PointLightComponent>()->rotates = false;
        fire.getComponent<PointLightComponent>()->casts_shadow = false;
        fire.getComponent<MaterialComponent>()->has_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.getComponent<TransformComponent>()->translation = glm::vec3{49.0f, 14.0f, -336.0f} + sponza_translation;
        fire.getComponent<PointLightComponent>()->rotates = false;
        fire.getComponent<PointLightComponent>()->casts_shadow = false;
        fire.getComponent<MaterialComponent>()->has_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.getComponent<TransformComponent>()->translation = glm::vec3{49.0f, -22.0f, -336.0f} + sponza_translation;
        fire.getComponent<PointLightComponent>()->rotates = false;
        fire.getComponent<PointLightComponent>()->casts_shadow = false;
        fire.getComponent<MaterialComponent>()->has_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }

    // lion eyes
    {
        VeGameObject green_eye = VeGameObject::createPointLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
        auto* tr = green_eye.getComponent<TransformComponent>();
        tr->translation = glm::vec3{126.7f, -5.87f, -331.2f} + sponza_translation;
        tr->scale = {.3f, .3f, .3f};
        green_eye.getComponent<PointLightComponent>()->rotates = false;
        green_eye.getComponent<PointLightComponent>()->casts_shadow = false;
        green_eye.getComponent<MaterialComponent>()->has_shadow = false;
        m_game_objects.emplace(green_eye.getId(), std::move(green_eye));
    }
    {
        VeGameObject green_eye = VeGameObject::createPointLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
        auto* tr = green_eye.getComponent<TransformComponent>();
        tr->translation = glm::vec3{126.7f, -1.24f, -331.2f} + sponza_translation;
        tr->scale = {.3f, .3f, .3f};
        green_eye.getComponent<PointLightComponent>()->rotates = false;
        green_eye.getComponent<PointLightComponent>()->casts_shadow = false;
        green_eye.getComponent<MaterialComponent>()->has_shadow = false;
        m_game_objects.emplace(green_eye.getId(), std::move(green_eye));
    }
}

} // namespace ve

