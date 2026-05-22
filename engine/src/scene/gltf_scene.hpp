#pragma once
#include "ve_export.hpp"
#include "scene/ve_scene.hpp"

namespace ve {

class VENGINE_API GltfScene : public VeScene {
public:
	// Empty scene with a default directional light
	explicit GltfScene(const SceneContext& ctx);

	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.04f}; }

private:
	void createDirectionalLight();
};

}