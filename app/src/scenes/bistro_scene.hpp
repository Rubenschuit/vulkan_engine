#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"
#include <memory>

namespace ve {

class VeModel;

class BistroScene : public VeScene {
public:
	BistroScene(const SceneContext& ctx, const AssetPaths& paths);
	BistroScene(const SceneContext& ctx, std::unique_ptr<VeModel> model);

	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.05f}; }

private:
	void loadGameObjects(const AssetPaths& paths);
	void setupScene(const glm::vec3& translation);

	std::unique_ptr<VeModel> m_bistro_model;
};

} // namespace ve
