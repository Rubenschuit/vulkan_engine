#pragma once
#include "../asset_paths.hpp"
#include "scene/ve_scene.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_material.hpp"
#include <memory>
#include <filesystem>

namespace ve {

class VeModel;

class BistroScene : public VeScene {
public:
	BistroScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths);

	~BistroScene() override {
		m_default_material_handle = ResourceHandle<VeMaterial>{};
	}

	vk::raii::DescriptorSet& getDescriptorSet() override;
	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.05f}; }

	void setSunIntensity(float intensity) override;
	float getSunIntensity() const override;
	Entity getSun() const override { return m_sun; }

private:
	void loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths);

	static constexpr float DEFAULT_SUN_INTENSITY = 5.0f;

	std::unique_ptr<VeModel> m_bistro_model;
	ResourceHandle<VeMaterial> m_default_material_handle;
	Entity m_sun;
};

} // namespace ve
