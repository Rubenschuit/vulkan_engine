#include "sponza_scene.hpp"
#include "game/ve_model.hpp"
#include "game/ve_component.hpp"

namespace ve {

SponzaScene::SponzaScene(VeDevice& device, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const std::filesystem::path& project_root)
    : VeScene(device, "Sponza Scene") {
    loadGameObjects(pool, material_layout, project_root);
}

vk::raii::DescriptorSet& SponzaScene::getDescriptorSet() {
    auto* model = m_game_objects.at(m_sponza_id).getComponent<ModelComponent>();
    return model->model->getMaterialDescriptorSet();
}

void SponzaScene::setSunIntensity(float intensity) {
    if (m_game_objects.contains(m_sun_id)) {
        auto* pl = m_game_objects.at(m_sun_id).getComponent<PointLightComponent>();
        if (pl) pl->intensity = intensity;
    }
}

void SponzaScene::loadGameObjects(VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const std::filesystem::path& project_root) {
    glm::vec3 sponza_translation = {0.0f, 0.0f, 300.0f};

    // Sponza model
    {
        VeGameObject sponza = VeGameObject::createGameObject();
        std::filesystem::path sponza_model_path = project_root / "models" / "sponza" / "glTF" / "Sponza.gltf";
		//std::filesystem::path sponza_model_path = project_root / "models" / "bistro-master" / "bistro.gltf";
        auto sponza_model = std::make_shared<VeModel>(m_device, sponza_model_path);
        sponza_model->createDescriptorSet(pool, material_layout);
        sponza.addComponent<ModelComponent>(sponza_model);
        auto* mat = sponza.addComponent<MaterialComponent>();
        mat->has_texture = 1.0f;
        m_sponza_id = sponza.getId();
        auto* transform = sponza.getComponent<TransformComponent>();
        transform->translation = glm::vec3{0.0f, 0.0f, -350.0f} + sponza_translation;
        transform->scale = {0.1f, 0.1f, 0.1f};
        m_game_objects.emplace(sponza.getId(), std::move(sponza));
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

