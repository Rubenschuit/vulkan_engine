#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"

namespace ve {

class BistroScene : public VeScene {
public:
	BistroScene(const SceneContext& ctx, const AssetPaths& paths);

	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.05f}; }
};

} // namespace ve
