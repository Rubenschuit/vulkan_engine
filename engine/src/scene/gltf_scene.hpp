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

	vk::raii::DescriptorSet& getDescriptorSet() override;
	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.04f}; }

private:
	void createDirectionalLight();

	vk::raii::DescriptorSet* m_fallback_descriptor_set = nullptr;
};

} // namespace ve