#include "sponza_scene.hpp"
#include <string>

namespace ve {

SponzaScene::SponzaScene(const SceneContext& ctx, const AssetPaths& paths)
    : VeScene(ctx, "Sponza Scene") {
    loadGameObjects(paths);
}

SponzaScene::SponzaScene(const SceneContext& ctx, std::unique_ptr<VeModel> model, const AssetPaths& paths)
    : VeScene(ctx, "Sponza Scene") {
    glm::vec3 sponza_translation = {0.0f, 0.0f, 300.0f};
    glm::vec3 root_translation = glm::vec3{0.0f, 0.0f, -350.0f} + sponza_translation;
    model->addToScene(m_registry, root_translation, {0.0f, 0.0f, 0.0f}, {12.5f, 12.5f, 12.5f});
    m_sponza_model = std::move(model);

    setupScene(sponza_translation, paths);
}

void SponzaScene::loadGameObjects(const AssetPaths& paths) {
    glm::vec3 sponza_translation = {0.0f, 0.0f, 300.0f};

    // Sponza model
    {
        std::filesystem::path sponza_model_path = paths.sponza_model();
        m_sponza_model = VeModel::load(m_resource_manager, sponza_model_path.lexically_normal());
        assert(m_sponza_model && "Failed to load Sponza model");

        glm::vec3 root_translation = glm::vec3{0.0f, 0.0f, -350.0f} + sponza_translation;
        glm::vec3 root_rotation = {0.0f, 0.0f, 0.0f};
        glm::vec3 root_scale = {12.5f, 12.5f, 12.5f};
        m_sponza_model->addToScene(m_registry, root_translation, root_rotation, root_scale);
    }

    setupScene(sponza_translation, paths);
}

void SponzaScene::setupScene(const glm::vec3& sponza_translation, const AssetPaths& paths) {
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
        pl->setRotates(rotates);
        pl->setCastsShadow(casts_shadow);
        return e;
    };

    // Directional light
    {
        Entity dl = m_registry.createDirectionalLight(3.0f, glm::vec3(1.0f),
            glm::normalize(glm::vec3(0.5f, -1.0f, -3.0f)));
        m_registry.setName(dl, "Directional Light");
        m_registry.getComponent<DirectionalLightComponent>(dl)->setCastsShadow(true);
    }

    ResourceHandle<VeTexture> fire_tex;
    ResourceHandle<VeTexture> smoke_tex;
    if (!paths.fire_texture.empty())
        fire_tex = m_resource_manager.load<VeTexture>(paths.fire_texture.lexically_normal().generic_string());
    if (!paths.smoke_texture.empty())
        smoke_tex = m_resource_manager.load<VeTexture>(paths.smoke_texture.lexically_normal().generic_string());

    auto attachFireEmitters = [&](Entity light) {
        {
            EmitterParams p{};
            p.color_start = glm::vec4(1.0f, 0.85f, 0.4f, 1.0f);
            p.color_end   = glm::vec4(1.0f, 0.2f, 0.02f, 0.0f);
            p.gravity     = -3.0f;
            p.drag        = 1.5f;
            p.stddev      = 1.0f;
            p.min_life    = 0.5f;
            p.max_life    = 1.2f;
            auto& ec = m_registry.addComponent<ParticleEmitterComponent>(light);
            ec.params = p;
            ec.texture = fire_tex;
            ec.rate = 40.0f;
        }
        {
            Entity smoke = m_registry.createEntity(m_registry.getName(light) + " Smoke");
            m_registry.addComponent<TransformComponent>(smoke);
            m_registry.setParent(smoke, light);
            EmitterParams p{};
            p.color_start = glm::vec4(0.7f, 0.65f, 0.6f, 0.9f);
            p.color_end   = glm::vec4(0.25f, 0.25f, 0.25f, 0.4f);
            p.gravity     = -1.0f;
            p.drag        = 0.8f;
            p.stddev      = 0.4f;
            p.min_life    = 3.0f;
            p.max_life    = 5.0f;
            p.atlas_one_shot = 1u;
            auto& ec = m_registry.addComponent<ParticleEmitterComponent>(smoke);
            ec.params = p;
            ec.texture = smoke_tex;
            ec.rate = 6.0f;
            ec.scale = 5.0f;
        }
    };

    const glm::vec3 fire_positions[4] = {
        glm::vec3{-62.0f, -14.5f, -336.0f} + sponza_translation,
        glm::vec3{-62.0f,  21.7f, -336.0f} + sponza_translation,
        glm::vec3{ 48.9f,  21.7f, -336.0f} + sponza_translation,
        glm::vec3{ 48.9f, -14.5f, -336.0f} + sponza_translation,
    };
    for (int i = 0; i < 4; ++i) {
        Entity e = makeLight(100.0f, 1.0f, glm::vec3(1.0f, .1f, .02f),
            "Fire " + std::to_string(i + 1), fire_positions[i], false, false);
        attachFireEmitters(e);
    }

    // lion eyes
    makeLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f), "Green eye (left)",
        glm::vec3{126.7f, -5.85f+7.3f, -331.2f} + sponza_translation, false, false,
        glm::vec3{.3f, .3f, .3f});
    makeLight(50.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f), "Green eye (right)",
        glm::vec3{126.7f, -1.22f + 7.3f, -331.2f} + sponza_translation, false, false,
        glm::vec3{.3f, .3f, .3f});
}

} // namespace ve
