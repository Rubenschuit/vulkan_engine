#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"

namespace ve {

class SponzaScene : public VeScene {
public:
	SponzaScene(const SceneContext& ctx, const AssetPaths& paths);

	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.04f}; }
};

}
