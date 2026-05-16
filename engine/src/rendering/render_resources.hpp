/* 
*  Long-lived GPU resources shared between RenderPipeline, AssetLoadingSystem,
*  and scene loading. 
*
*  Constructor builds layouts/pool/per-frame buffers/default material and
*  particle textures. bindMaterialSsbo() must be called after 
*  SceneResourceManager exists, before render systems are constructed. It passes 
*  the global descriptor sets to the material SSBO.
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
struct EngineConfig;
struct SceneContext;

class VENGINE_API RenderResources {
public:
	static constexpr uint32_t INITIAL_INSTANCE_CAPACITY = 16384 * 2;

	RenderResources(VeDevice& device,
	                VeResourceManager& resource_manager,
	                const EngineConfig& config);
	~RenderResources();

	RenderResources(const RenderResources&) = delete;
	RenderResources& operator=(const RenderResources&) = delete;

	void bindMaterialSsbo(const VeBuffer& material_ssbo);

	std::shared_ptr<VeDescriptorPool> pool() const { return m_global_pool; }
	VeDescriptorSetLayout& globalSetLayout() { return *m_global_set_layout; }
	VeDescriptorSetLayout& materialSetLayout() { return *m_material_set_layout; }

	std::vector<std::unique_ptr<VeBuffer>>& uniformBuffers() { return m_uniform_buffers; }
	std::vector<std::unique_ptr<VeBuffer>>& instanceBuffers() { return m_instance_buffers; }
	std::vector<vk::raii::DescriptorSet>& globalDescriptorSets() { return m_global_descriptor_sets; }
	vk::raii::DescriptorSet& globalDescriptorSet(uint32_t frame) { return m_global_descriptor_sets[frame]; }

	vk::raii::DescriptorSet& defaultMaterialDescriptorSet() { return m_default_material_descriptor_set; }

	ResourceHandle<VeTexture> particleTexture() const { return m_particle_texture_handle; }
	ResourceHandle<VeTexture> fireTexture() const { return m_fire_texture_handle; }
	ResourceHandle<VeTexture> smokeTexture() const { return m_smoke_texture_handle; }

	SceneContext makeSceneContext();

private:
	void createBuffers();
	void createDescriptors();

	VeDevice& m_ve_device;
	VeResourceManager& m_resource_manager;
	const EngineConfig& m_config;

	std::vector<std::unique_ptr<VeBuffer>> m_uniform_buffers;
	std::vector<std::unique_ptr<VeBuffer>> m_instance_buffers;

	std::shared_ptr<VeDescriptorPool> m_global_pool;
	std::unique_ptr<VeDescriptorSetLayout> m_global_set_layout;
	std::unique_ptr<VeDescriptorSetLayout> m_material_set_layout;
	std::vector<vk::raii::DescriptorSet> m_global_descriptor_sets;

	std::unique_ptr<VeBuffer> m_default_material_ubo;
	ResourceHandle<VeTexture> m_default_albedo_handle;
	ResourceHandle<VeTexture> m_default_normal_handle;
	ResourceHandle<VeTexture> m_default_mr_handle;
	ResourceHandle<VeTexture> m_default_occlusion_handle;
	ResourceHandle<VeTexture> m_default_emissive_handle;
	vk::raii::DescriptorSet m_default_material_descriptor_set{nullptr};

	ResourceHandle<VeTexture> m_particle_texture_handle;
	ResourceHandle<VeTexture> m_fire_texture_handle;
	ResourceHandle<VeTexture> m_smoke_texture_handle;
};

}