#pragma once
#include "../asset_paths.hpp"
#include "scene/ve_scene.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_resource_manager.hpp"
#include <memory>
#include <filesystem>

namespace ve {

class SimpleScene : public VeScene {
public:

	SimpleScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, vk::raii::DescriptorSet* default_material_descriptor_set);

	vk::raii::DescriptorSet& getDescriptorSet() override { return *m_default_material_descriptor_set; }
	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.02f}; }

	void setSunIntensity(float intensity) override;
	float getSunIntensity() const override;
	Entity getSun() const override { return m_sun; }

private:
	void loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths);

	static constexpr float DEFAULT_SUN_INTENSITY = 3.0f;

	vk::raii::DescriptorSet* m_default_material_descriptor_set = nullptr;

	Entity m_sun;
};

}

