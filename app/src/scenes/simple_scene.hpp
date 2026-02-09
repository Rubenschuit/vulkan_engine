#pragma once
#include "../asset_paths.hpp"
#include "game/ve_scene.hpp"
#include "core/ve_descriptors.hpp"
#include "core/ve_resource_manager.hpp"
#include <memory>
#include <filesystem>

namespace ve {

class SimpleScene : public VeScene {
public:
	// shared_particle_descriptor_set: descriptor set for particles/point lights, owned by Sandbox
	SimpleScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, vk::raii::DescriptorSet* shared_particle_descriptor_set);

	vk::raii::DescriptorSet& getDescriptorSet() override { return *m_shared_particle_descriptor_set; }
	Type getType() const override { return Type::SIMPLE; }

private:
	void loadGameObjects(VeResourceManager& resource_manager, const AssetPaths& paths);

	vk::raii::DescriptorSet* m_shared_particle_descriptor_set = nullptr;
};

}

