#pragma once
#include "VEngine/VEngine.hpp"
#include <filesystem>

namespace ve {

class GltfScene : public VeScene {
public:
	// Empty scene (just directional light, no model).
	GltfScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout,
			  vk::raii::DescriptorSet* fallback_descriptor_set);

	// Scene with an initial model.
	GltfScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout,
			  const std::filesystem::path& gltf_path, vk::raii::DescriptorSet* fallback_descriptor_set);

	vk::raii::DescriptorSet& getDescriptorSet() override;
	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.04f}; }

private:
	void createDirectionalLight();

	vk::raii::DescriptorSet* m_fallback_descriptor_set = nullptr;
};

} // namespace ve
