#include "sponza_scene.hpp"
#include "game/ve_model.hpp"

namespace ve {

SponzaScene::SponzaScene(VeDevice& device, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const std::filesystem::path& project_root)
    : VeScene(device, "Sponza Scene") {
    loadGameObjects(pool, material_layout, project_root);
}

vk::raii::DescriptorSet& SponzaScene::getDescriptorSet() {
    return m_game_objects.at(m_sponza_id).ve_model->getMaterialDescriptorSet();
}

void SponzaScene::setSunIntensity(float intensity) {
    if (m_game_objects.contains(m_sun_id)) {
        m_game_objects.at(m_sun_id).point_light_component->intensity = intensity;
    }
}

void SponzaScene::loadGameObjects(VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const std::filesystem::path& project_root) {
    glm::vec3 sponza_translation = {0.0f, 0.0f, 300.0f};

    // Sponza model
    {
        VeGameObject sponza = VeGameObject::createGameObject();
        std::filesystem::path sponza_model_path = project_root / "models" / "sponza" / "glTF" / "Sponza.gltf";
        auto sponza_model = std::make_shared<VeModel>(m_device, sponza_model_path);
        sponza_model->createDescriptorSet(pool, material_layout);
        sponza.ve_model = sponza_model;
        m_sponza_id = sponza.getId();
        sponza.transform.translation = glm::vec3{0.0f, 0.0f, -350.0f} + sponza_translation;
        sponza.transform.scale = {0.1f, 0.1f, 0.1f};
        sponza.has_texture = 1.0f;
        m_game_objects.emplace(sponza.getId(), std::move(sponza));
    }

    // sponza sun light
    {
        VeGameObject sun = VeGameObject::createPointLight(2000.0f, 4.0f, glm::vec3(1.0f, 1.0f, 1.0f));
        sun.transform.translation = glm::vec3{0.0f, 50.0f, -140.0f} + sponza_translation;
        sun.point_light_component->rotates = true;
        m_sun_id = sun.getId();
        m_game_objects.emplace(sun.getId(), std::move(sun));
    }

    // sponza fire lights
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.transform.translation = glm::vec3{-62.0f, -22.0f, -336.0f} + sponza_translation;
        fire.point_light_component->rotates = false;
        fire.point_light_component->casts_shadow = false;
        fire.has_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.transform.translation = glm::vec3{-62.0f, 14.0f, -336.0f} + sponza_translation;
        fire.point_light_component->rotates = false;
        fire.point_light_component->casts_shadow = false;
        fire.has_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.transform.translation = glm::vec3{49.0f, 14.0f, -336.0f} + sponza_translation;
        fire.point_light_component->rotates = false;
        fire.point_light_component->casts_shadow = false;
        fire.has_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }
    {
        VeGameObject fire = VeGameObject::createPointLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f));
        fire.transform.translation = glm::vec3{49.0f, -22.0f, -336.0f} + sponza_translation;
        fire.point_light_component->rotates = false;
        fire.point_light_component->casts_shadow = false;
        fire.has_shadow = false;
        m_game_objects.emplace(fire.getId(), std::move(fire));
    }

    // lion eyes
    {
        VeGameObject green_eye = VeGameObject::createPointLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
        green_eye.transform.translation = glm::vec3{126.7f, -5.87f, -331.2f} + sponza_translation;
        green_eye.transform.scale = {.3f, .3f, .3f};
        green_eye.point_light_component->rotates = false;
        green_eye.point_light_component->casts_shadow = false;
        green_eye.has_shadow = false;
        m_game_objects.emplace(green_eye.getId(), std::move(green_eye));
    }
    {
        VeGameObject green_eye = VeGameObject::createPointLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
        green_eye.transform.translation = glm::vec3{126.7f, -1.24f, -331.2f} + sponza_translation;
        green_eye.transform.scale = {.3f, .3f, .3f};
        green_eye.point_light_component->rotates = false;
        green_eye.point_light_component->casts_shadow = false;
        green_eye.has_shadow = false;
        m_game_objects.emplace(green_eye.getId(), std::move(green_eye));
    }
}

} // namespace ve

