#pragma once
#include "scene/ve_scene.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_material.hpp"
#include <memory>
#include <filesystem>
#include <vector>

namespace ve {

class VeModel;

class GltfScene : public VeScene {
public:
	// Empty scene (just sun light, no model).
	GltfScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout,
			  vk::raii::DescriptorSet* fallback_descriptor_set);

	// Scene with an initial model.
	GltfScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout,
			  const std::filesystem::path& gltf_path, vk::raii::DescriptorSet* fallback_descriptor_set);

	~GltfScene() override {
		m_default_material_handle = ResourceHandle<VeMaterial>{};
	}

	// Load a GLTF model and add it to the scene.
	void addModel(const std::filesystem::path& gltf_path);

	vk::raii::DescriptorSet& getDescriptorSet() override;
	Type getType() const override { return Type::PBR; }

	void setSunIntensity(float intensity) override;
	float getSunIntensity() const override;
	Entity getSun() const override { return m_sun; }

private:
	void createSunLight();

	static constexpr float DEFAULT_SUN_INTENSITY = 2000.0f;

	VeResourceManager& m_resource_manager;
	VeDescriptorPool& m_pool;
	VeDescriptorSetLayout& m_material_layout;

	std::vector<std::unique_ptr<VeModel>> m_models;
	ResourceHandle<VeMaterial> m_default_material_handle;
	Entity m_sun;
	vk::raii::DescriptorSet* m_fallback_descriptor_set = nullptr;
};

} // namespace ve
