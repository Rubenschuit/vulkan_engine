/*
*  Long-lived, scene-agnostic GPU infrastructure: descriptor pool, global and
*  material set layouts, default-material descriptor set + UBO + textures, and
*  the engine-default particle texture.
*/
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <vector>

namespace ve {

class VeDevice;
class VeBuffer;
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

	vk::raii::DescriptorSet& defaultMaterialDescriptorSet() { return m_default_material_descriptor_set; }

	ResourceHandle<VeTexture> defaultParticleTexture() const { return m_default_particle_texture_handle; }

	SceneContext makeSceneContext();

private:
	void createDescriptors();

	VeDevice& m_ve_device;
	VeResourceManager& m_resource_manager;

	std::shared_ptr<VeDescriptorPool> m_global_pool;
	std::unique_ptr<VeDescriptorSetLayout> m_global_set_layout;
	std::unique_ptr<VeDescriptorSetLayout> m_material_set_layout;

	std::unique_ptr<VeBuffer> m_default_material_ubo;
	ResourceHandle<VeTexture> m_default_albedo_handle;
	ResourceHandle<VeTexture> m_default_normal_handle;
	ResourceHandle<VeTexture> m_default_mr_handle;
	ResourceHandle<VeTexture> m_default_occlusion_handle;
	ResourceHandle<VeTexture> m_default_emissive_handle;
	vk::raii::DescriptorSet m_default_material_descriptor_set{nullptr};

	ResourceHandle<VeTexture> m_default_particle_texture_handle;
};

}