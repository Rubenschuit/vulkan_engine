#include "sponza_scene.hpp"
#include "resources/ve_model.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_game_object.hpp"

namespace ve {

SponzaScene::SponzaScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, const char* variant)
    : VeScene(device, "Sponza Scene") {
    loadGameObjects(resource_manager, pool, material_layout, paths, variant);
}

vk::raii::DescriptorSet& SponzaScene::getDescriptorSet() {
    assert(m_default_material_handle.isValid() && m_default_material_handle.get()->hasDescriptorSet() && "SponzaScene requires at least one textured material");
    return m_default_material_handle.get()->getDescriptorSet();
}



void SponzaScene::setSunIntensity(float intensity) {
    if (m_game_objects.contains(m_sun_id)) {
        auto* pl = m_game_objects.at(m_sun_id).getComponent<PointLightComponent>();
        if (pl) pl->intensity = intensity;
    }
}

float SponzaScene::getSunIntensity() const {
    if (m_game_objects.contains(m_sun_id)) {
        const auto* pl = m_game_objects.at(m_sun_id).getComponent<PointLightComponent>();
        return pl ? pl->intensity : DEFAULT_SUN_INTENSITY;
    }
    return DEFAULT_SUN_INTENSITY;
}

void SponzaScene::loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, const char* variant) {
    glm::vec3 sponza_translation = {0.0f, 0.0f, 300.0f};

    // Sponza model (variant: sponza, sponza_low, sponza_high)
    {
        std::filesystem::path sponza_model_path = paths.sponza_model(variant);
        m_sponza_model = VeModel::load(resource_manager, sponza_model_path.lexically_normal(), &pool, &material_layout);
        assert(m_sponza_model && "Failed to load Sponza model");

        glm::vec3 root_translation = glm::vec3{0.0f, 0.0f, -350.0f} + sponza_translation;
        glm::vec3 root_rotation = {0.0f, 0.0f, 0.0f};
        glm::vec3 root_scale = {12.5f, 12.5f, 12.5f};
        m_sponza_model->addToScene(m_game_objects, root_translation, root_rotation, root_scale);

        // Store default material for getDescriptorSet() fallback
        for (auto& [id, obj] : m_game_objects) {
            auto* mesh = obj.getComponent<MeshComponent>();
            if (mesh && mesh->hasMaterial() && mesh->getMaterial()->hasDescriptorSet()) {
                m_default_material_handle = mesh->getMaterialHandle();
                break;
            }
        }

        // Set has_texture for all mesh objects (textured Sponza materials)
        for (auto& [id, obj] : m_game_objects) {
            if (auto* mesh = obj.getComponent<MeshComponent>()) {
                mesh->has_texture = 1.0f;
            }
        }
    }

    // sponza sun light
    {
        VeGameObject sun = VeGameObject::createPointLight(DEFAULT_SUN_INTENSITY, 4.0f, glm::vec3(1.0f, 1.0f, 1.0f));
        sun.setName("Sun");
        sun.getComponent<TransformComponent>()->setTranslation(glm::vec3{0.0f, 50.0f, -140.0f} + sponza_translation);
        sun.getComponent<PointLightComponent>()->rotates = true;
        sun.getComponent<PointLightComponent>()->casts_shadow = true;
        m_sun_id = sun.getId();
        m_game_objects.emplace(sun.getId(), std::move(sun));
    }

    // sponza fire lights
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.setName("Fire 1");
        fire.getComponent<TransformComponent>()->setTranslation(glm::vec3{-62.0f, -14.5f, -336.0f} + sponza_translation);
        fire.getComponent<PointLightComponent>()->rotates = false;
        fire.getComponent<PointLightComponent>()->casts_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.setName("Fire 2");
        fire.getComponent<TransformComponent>()->setTranslation(glm::vec3{-62.0f, 21.7f, -336.0f} + sponza_translation);
        fire.getComponent<PointLightComponent>()->rotates = false;
        fire.getComponent<PointLightComponent>()->casts_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.setName("Fire 3");
        fire.getComponent<TransformComponent>()->setTranslation(glm::vec3{48.9f, 21.7f, -336.0f} + sponza_translation);
        fire.getComponent<PointLightComponent>()->rotates = false;
        fire.getComponent<PointLightComponent>()->casts_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.setName("Fire 4");
        fire.getComponent<TransformComponent>()->setTranslation(glm::vec3{48.9f, -14.5f, -336.0f} + sponza_translation);
        fire.getComponent<PointLightComponent>()->rotates = false;
        fire.getComponent<PointLightComponent>()->casts_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }

    // lion eyes
    {
        VeGameObject green_eye = VeGameObject::createPointLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
        green_eye.setName("Green eye (left)");
        auto* tr = green_eye.getComponent<TransformComponent>();
        tr->setTranslation(glm::vec3{126.7f, -5.85f+7.3f, -331.2f} + sponza_translation);
        tr->setScale({.3f, .3f, .3f});
        green_eye.getComponent<PointLightComponent>()->rotates = false;
        green_eye.getComponent<PointLightComponent>()->casts_shadow = false;
        m_game_objects.emplace(green_eye.getId(), std::move(green_eye));
    }
    {
        VeGameObject green_eye = VeGameObject::createPointLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
        green_eye.setName("Green eye (right)");
        auto* tr = green_eye.getComponent<TransformComponent>();
        tr->setTranslation(glm::vec3{126.7f, -1.22f + 7.3f, -331.2f} + sponza_translation);
        tr->setScale({.3f, .3f, .3f});
        green_eye.getComponent<PointLightComponent>()->rotates = false;
        green_eye.getComponent<PointLightComponent>()->casts_shadow = false;
        m_game_objects.emplace(green_eye.getId(), std::move(green_eye));
    }
}

} // namespace ve

