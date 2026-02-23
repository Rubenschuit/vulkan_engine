#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"
#include <memory>

namespace ve {

class VeModel;

class BistroScene : public VeScene {
public:
	BistroScene(const SceneContext& ctx, const AssetPaths& paths);

	vk::raii::DescriptorSet& getDescriptorSet() override;
	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.05f}; }

private:
	void loadGameObjects(const AssetPaths& paths);

	std::unique_ptr<VeModel> m_bistro_model;
};

} // namespace ve
