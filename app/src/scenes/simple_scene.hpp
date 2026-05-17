#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"

namespace ve {

class SimpleScene : public VeScene {
public:
	SimpleScene(const SceneContext& ctx, const AssetPaths& paths);

	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.02f}; }

private:
	void loadGameObjects(const AssetPaths& paths);
};

}
