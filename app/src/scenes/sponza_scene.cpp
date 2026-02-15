#include "sponza_scene.hpp"
#include "resources/ve_model.hpp"
#include "scene/ve_component.hpp"

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
    auto* pl = m_registry.getComponent<PointLightComponent>(m_sun);
    if (pl) pl->intensity = intensity;
}

float SponzaScene::getSunIntensity() const {
    const auto* pl = m_registry.getComponent<PointLightComponent>(m_sun);
    return pl ? pl->intensity : DEFAULT_SUN_INTENSITY;
}

void SponzaScene::loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, const char* variant) {
    glm::vec3 sponza_translation = {0.0f, 0.0f, 300.0f};

    // Helper: create a point light entity in Registry
    auto makeLight = [&](float intensity, float radius, glm::vec3 color, const std::string& name,
                         glm::vec3 pos, bool rotates, bool casts_shadow,
                         glm::vec3 light_scale = glm::vec3(-1.0f)) -> Entity {
        Entity e = m_registry.createPointLight(intensity, radius, color);
        m_registry.setName(e, name);
        auto* tc = m_registry.getComponent<TransformComponent>(e);
        tc->setTranslation(pos);
        if (light_scale.x >= 0.0f) tc->setScale(light_scale);
        auto* pl = m_registry.getComponent<PointLightComponent>(e);
        pl->rotates = rotates;
        pl->casts_shadow = casts_shadow;
        return e;
    };

    // Sponza model (variant: sponza, sponza_low, sponza_high)
    {
        std::filesystem::path sponza_model_path = paths.sponza_model(variant);
        m_sponza_model = VeModel::load(resource_manager, sponza_model_path.lexically_normal(), &pool, &material_layout);
        assert(m_sponza_model && "Failed to load Sponza model");

        glm::vec3 root_translation = glm::vec3{0.0f, 0.0f, -350.0f} + sponza_translation;
        glm::vec3 root_rotation = {0.0f, 0.0f, 0.0f};
        glm::vec3 root_scale = {12.5f, 12.5f, 12.5f};
        m_sponza_model->addToScene(m_registry, root_translation, root_rotation, root_scale);

        // Store default material for getDescriptorSet() fallback
        for (auto& mc : m_registry.meshes()) {
            if (mc.hasMaterial() && mc.getMaterial()->hasDescriptorSet()) {
                m_default_material_handle = mc.getMaterialHandle();
                break;
            }
        }
    }

    // sponza sun light
    {
        Entity sun = makeLight(DEFAULT_SUN_INTENSITY, 4.0f, glm::vec3(1.0f), "Sun",
            glm::vec3{0.0f, 50.0f, -140.0f} + sponza_translation, true, true);
        m_sun = sun;
    }

    // sponza fire lights
    makeLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f), "Fire 1",
        glm::vec3{-62.0f, -14.5f, -336.0f} + sponza_translation, false, false);
    makeLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f), "Fire 2",
        glm::vec3{-62.0f, 21.7f, -336.0f} + sponza_translation, false, false);
    makeLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f), "Fire 3",
        glm::vec3{48.9f, 21.7f, -336.0f} + sponza_translation, false, false);
    makeLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f), "Fire 4",
        glm::vec3{48.9f, -14.5f, -336.0f} + sponza_translation, false, false);

    // lion eyes
    makeLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f), "Green eye (left)",
        glm::vec3{126.7f, -5.85f+7.3f, -331.2f} + sponza_translation, false, false,
        glm::vec3{.3f, .3f, .3f});
    makeLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f), "Green eye (right)",
        glm::vec3{126.7f, -1.22f + 7.3f, -331.2f} + sponza_translation, false, false,
        glm::vec3{.3f, .3f, .3f});
}

} // namespace ve
