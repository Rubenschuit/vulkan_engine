#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"

#include <memory>
#include <vector>
#include <filesystem>

namespace ve {
	class VeGameObject;
	class VeDevice;
	class VePipeline;
	class MeshComponent;
    class VeBuffer;
    class VeDescriptorPool;
    class VeDescriptorSetLayout;
    class VeImage;
}

namespace ve {

class VENGINE_API ShadowRenderSystem {
public:
	ShadowRenderSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& material_set_layout,
		std::filesystem::path shader_path);
	~ShadowRenderSystem();

	ShadowRenderSystem(const ShadowRenderSystem&) = delete;
	ShadowRenderSystem& operator=(const ShadowRenderSystem&) = delete;

	// Update shadow UBO with light data from main UBO
	void updateUniformBuffer(uint32_t frame_index, UniformBufferObject& ubo);

	// Render all shadow maps for all lights
	void renderShadowMaps(VeFrameInfo& frame_info);

	// Get shadow descriptor set for a specific frame
	vk::raii::DescriptorSet& getShadowDescriptorSet(uint32_t frame_index) {
		return m_shadow_descriptor_sets[frame_index];
	}

	// Get shadow descriptor set layout
	const vk::raii::DescriptorSetLayout& getShadowSetLayout() const {
		return m_shadow_set_layout->getDescriptorSetLayout();
	}

	// Invalidate cached shadow drawables (e.g. after scene switch); next render will rebuild.
	void invalidateShadowDrawables();

private:
	void createShadowResources();
	void createPipelineLayout(
		const vk::raii::DescriptorSetLayout& material_set_layout);
	void createPipeline(vk::Format depth_format);
	void createShadowUBOs();
	void createShadowPassDescriptorSets(VeDescriptorPool& descriptor_pool);
	void createShadowTextureDescriptorSets(VeDescriptorPool& descriptor_pool);
	void renderShadowMap(VeFrameInfo& frame_info, uint32_t light_index) const;

	VeDevice& m_ve_device;

	std::filesystem::path m_shader_path;

	// Shadow global descriptor set layout (for shadow pass UBO)
	std::unique_ptr<VeDescriptorSetLayout> m_shadow_global_set_layout;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;

	// Shadow-specific UBOs (per-frame, per-light)
	std::vector<std::vector<std::unique_ptr<VeBuffer>>> m_shadow_ubos;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_global_descriptor_sets;

	// Cached light views and projections (per-frame)
	std::vector<std::vector<glm::mat4>> m_light_views;
	std::vector<std::vector<glm::mat4>> m_light_projs;

	// Shadow mapping resources
	std::unique_ptr<VeImage> m_shadow_map_array;  // single 2D array texture with MAX_LIGHTS layers
	std::vector<vk::raii::ImageView> m_shadow_map_layer_views;  // individual layer views for rendering
	vk::raii::Sampler m_shadow_sampler{nullptr};
	std::unique_ptr<VeDescriptorSetLayout> m_shadow_set_layout;
	std::vector<vk::raii::DescriptorSet> m_shadow_descriptor_sets;  // per frame, for binding shadow maps
	vk::Format m_shadow_depth_format;

	struct ShadowDrawable {
		VeGameObject* obj = nullptr;
		MeshComponent* mesh = nullptr;
	};
	std::vector<ShadowDrawable> m_shadow_drawables;
	bool m_shadow_drawables_dirty = true;
};

}