#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"
#include <memory>

namespace ve {

class VeModel;

class SponzaScene : public VeScene {
public:
    // variant: selects KTX2 quality preset (sponza or sponza_low (ETC1S))
    SponzaScene(const SceneContext& ctx, const AssetPaths& paths, const char* variant = "sponza");

    vk::raii::DescriptorSet& getDescriptorSet() override;
    glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.04f}; }

private:
    void loadGameObjects(const AssetPaths& paths, const char* variant);

    std::unique_ptr<VeModel> m_sponza_model;
};

}

