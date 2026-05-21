/*
*  Long-lived, scene-agnostic GPU infrastructure: descriptor pool, global and
*  material set layouts, and the engine-default particle texture.
*/
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

#include <memory>

namespace ve {

class VeDevice;
class VeDescriptorPool;
class VeDescriptorSetLayout;
struct SceneContext;

class VENGINE_API RenderResources {
public:
	RenderResources(VeDevice& device,
	                VeResourceManager& resource_manager);
	~RenderResources();

	RenderResources(const RenderResources&) = delete;
	RenderResources& operator=(const RenderResources&) = delete;

	std::shared_ptr<VeDescriptorPool> pool() const { return m_global_pool; }
	VeDescriptorSetLayout& globalSetLayout() { return *m_global_set_layout; }
	VeDescriptorSetLayout& materialSetLayout() { return *m_material_set_layout; }

	ResourceHandle<VeTexture> defaultParticleTexture() const { return m_default_particle_texture_handle; }

	SceneContext makeSceneContext();

private:
	void createDescriptors();

	VeDevice& m_ve_device;
	VeResourceManager& m_resource_manager;

	std::shared_ptr<VeDescriptorPool> m_global_pool;
	std::unique_ptr<VeDescriptorSetLayout> m_global_set_layout;
	std::unique_ptr<VeDescriptorSetLayout> m_material_set_layout;

	ResourceHandle<VeTexture> m_default_particle_texture_handle;
};

}
