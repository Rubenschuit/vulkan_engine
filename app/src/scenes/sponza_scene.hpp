#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"
#include <memory>

namespace ve {

class VeModel;

class SponzaScene : public VeScene {
public:
    SponzaScene(const SceneContext& ctx, const AssetPaths& paths);
    SponzaScene(const SceneContext& ctx, std::unique_ptr<VeModel> model);

    vk::raii::DescriptorSet& getDescriptorSet() override;
    glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.04f}; }
	SceneSubsystems declareSubsystems() const override {
		return {.particles = std::nullopt, .fireworks = FireworksSceneConfig{}};
	}

private:
    void loadGameObjects(const AssetPaths& paths);
    void setupScene(const glm::vec3& sponza_translation);

    std::unique_ptr<VeModel> m_sponza_model;
};

}

