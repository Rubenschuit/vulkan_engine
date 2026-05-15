#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"

namespace ve {

class SimpleScene : public VeScene {
public:

	SimpleScene(const SceneContext& ctx, const AssetPaths& paths);

	vk::raii::DescriptorSet& getDescriptorSet() override { return *m_default_material_descriptor_set; }
	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.02f}; }
	SceneSubsystems declareSubsystems() const override {
		return {.particles = ParticleSceneConfig{}, .fireworks = FireworksSceneConfig{}};
	}

private:
	void loadGameObjects(const AssetPaths& paths);

	vk::raii::DescriptorSet* m_default_material_descriptor_set = nullptr;
};

}

