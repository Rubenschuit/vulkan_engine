#pragma once
#include "ve_export.hpp"
#include "scene/ve_scene.hpp"
#include <filesystem>

namespace ve {

class VENGINE_API GltfScene : public VeScene {
public:
	// Empty scene (just directional light, no model).
	explicit GltfScene(const SceneContext& ctx);

	// Scene with an initial model.
	GltfScene(const SceneContext& ctx, const std::filesystem::path& gltf_path);

	// Scene from a pre-loaded model (async loading path).
	GltfScene(const SceneContext& ctx, std::unique_ptr<VeModel> model, const std::string& name);

	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.04f}; }

private:
	void createDirectionalLight();
};

} // namespace ve