#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"

#include <memory>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <filesystem>

namespace ve {
	class VeDevice;
	class VePipeline;
	class VeMesh;
	class MeshComponent;
	class VeBuffer;
	class VeDescriptorPool;
	class VeDescriptorSetLayout;
	class VeImage;
	class Registry;
	class GpuCullingSystem;
	class MeshletCullingSystem;
	class PbrMegaBuffer;
	class GpuSceneManager;
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

	// Update shadow UBOs with light data from main UBO and CSM cascade data
	void updateUniformBuffer(uint32_t frame_index, UniformBufferObject& ubo,
	                         const CsmCascadeData& csm_data);

	// Render all shadow maps for all lights (CPU-driven path)
	void renderShadowMaps(VeFrameInfo& frame_info);
	void renderShadowMapsGpuCulled(VeFrameInfo& frame_info,
	                               GpuCullingSystem& gpu_cull,
	                               PbrMegaBuffer& mega_buffer,
	                               GpuSceneManager& scene_mgr);

	void createGpuShadowDescriptorSets(GpuCullingSystem& gpu_cull);

	void createMeshletShadowDescriptorSets(MeshletCullingSystem& meshlet_cull);
	void renderShadowMapsGpuCulledMeshlets(VeFrameInfo& frame_info,
	                                       MeshletCullingSystem& meshlet_cull,
	                                       PbrMegaBuffer& mega_buffer,
	                                       GpuSceneManager& scene_mgr);

	// Get shadow descriptor set for a specific frame
	vk::raii::DescriptorSet& getShadowDescriptorSet(uint32_t frame_index) {
		return m_shadow_descriptor_sets[frame_index];
	}

	// Get shadow descriptor set layout
	const vk::raii::DescriptorSetLayout& getShadowSetLayout() const {
		return m_shadow_set_layout->getDescriptorSetLayout();
	}

	// Atlas layout accessors (needed by LightSystem for bias matrix computation)
	const std::array<FrameAtlasRegion, MAX_SHADOW_LAYERS>& getAtlasRegions() const { return m_atlas_regions; }
	uint32_t getAtlasWidth() const { return m_atlas_width; }
	uint32_t getAtlasHeight() const { return m_atlas_height; }

	// Invalidate cached shadow drawables (e.g. after scene switch); next render will rebuild.
	void invalidateShadowDrawables();

	// Subscribe to registry events for automatic cache invalidation.
	void subscribeToRegistry(Registry& registry);

private:
	struct ShadowDrawable {
		Entity entity;
		MeshComponent* mesh = nullptr;
		uint32_t lod_level = 0;
	};

	struct ShadowInstanceGroup {
		VeMesh* mesh = nullptr;
		uint32_t lod_level = 0;
		uint32_t first_instance = 0;
		uint32_t instance_count = 0;
	};

	void createShadowResources();
	void createPipelineLayout(
		const vk::raii::DescriptorSetLayout& material_set_layout);
	void createPipeline(vk::Format depth_format);
	void computeAtlasLayout();
	void createShadowUBOs();
	void createShadowInstanceBuffers();
	void createShadowPassDescriptorSets(VeDescriptorPool& descriptor_pool);
	void createShadowTextureDescriptorSets(VeDescriptorPool& descriptor_pool);
	void renderShadowMap(VeFrameInfo& frame_info, uint32_t light_index,
		const std::vector<ShadowInstanceGroup>& instance_groups) const;
	void rebuildMegaBuffers(vk::raii::CommandBuffer& cmd, const std::vector<VeMesh*>& unique_meshes);
	void growShadowInstanceBuffers(uint32_t new_capacity);

	VeDevice& m_ve_device;
	VeDescriptorPool& m_descriptor_pool;

	std::filesystem::path m_shader_path;

	// Shadow global descriptor set layout (for shadow pass UBO + instance SSBO)
	std::unique_ptr<VeDescriptorSetLayout> m_shadow_global_set_layout;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;

	// Point light shadow UBOs (per-frame, per-point-light)
	std::vector<std::vector<std::unique_ptr<VeBuffer>>> m_shadow_ubos;  // [frame][point_light]
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_global_descriptor_sets;  // [frame][point_light]

	// Cached light views and projections per active layer (per-frame)
	std::vector<std::vector<glm::mat4>> m_light_views;  // [frame][layer]
	std::vector<std::vector<glm::mat4>> m_light_projs;  // [frame][layer]

	// Shadow atlas
	std::array<FrameAtlasRegion, MAX_SHADOW_LAYERS> m_atlas_regions{};
	uint32_t m_atlas_width = 0;
	uint32_t m_atlas_height = 0;
	std::unique_ptr<VeImage> m_shadow_atlas;  // single 2D depth image
	vk::raii::Sampler m_shadow_sampler{nullptr};      // comparison sampler (SampleCmpLevelZero)
	vk::raii::Sampler m_shadow_raw_sampler{nullptr};   // regular sampler for raw depth reads (PCSS)
	std::unique_ptr<VeDescriptorSetLayout> m_shadow_set_layout;
	std::vector<vk::raii::DescriptorSet> m_shadow_descriptor_sets;  // per frame, for binding shadow maps
	vk::Format m_shadow_depth_format;

	// Dedicated shadow instance SSBO (not shared with main render pass)
	static constexpr uint32_t INITIAL_SHADOW_INSTANCE_CAPACITY = 16384;
	uint32_t m_shadow_instance_capacity = INITIAL_SHADOW_INSTANCE_CAPACITY;
	std::vector<std::unique_ptr<VeBuffer>> m_shadow_instance_buffers; // per-frame

	std::vector<ShadowDrawable> m_shadow_drawables;
	bool m_shadow_drawables_dirty = true;
	std::vector<VeMesh*> m_cached_unique_meshes; // sorted unique meshes for mega-buffer change detection

	std::vector<ShadowInstanceGroup> m_shadow_instance_groups; // point light shadows
	std::vector<ShadowInstanceGroup> m_csm_instance_groups; // CSM (shared across all views)

	// Mega-buffer: all shadow geometry consolidated into single VBO+IBO
	std::unique_ptr<VeBuffer> m_mega_shadow_vbo;
	std::unique_ptr<VeBuffer> m_mega_ibo;
	uint32_t m_mega_total_vertices = 0;
	uint32_t m_mega_total_indices = 0;

	struct LodMegaEntry {
		uint32_t first_index;
		uint32_t index_count;
	};
	struct MeshMegaEntry {
		uint32_t vertex_offset;
		std::vector<LodMegaEntry> lod_entries;
	};
	std::unordered_map<VeMesh*, MeshMegaEntry> m_mega_entries;

	// GPU-culled CSM
	std::vector<std::vector<vk::raii::DescriptorSet>> m_gpu_cascade_descriptor_sets;
	std::vector<std::vector<std::unique_ptr<VeBuffer>>> m_csm_cascade_ubos;

	// GPU-culled point/spot lights
	std::vector<std::vector<vk::raii::DescriptorSet>> m_gpu_shadow_descriptor_sets;

	// Meshlet-culled CSM
	std::vector<std::vector<vk::raii::DescriptorSet>> m_meshlet_cascade_descriptor_sets;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_meshlet_shadow_descriptor_sets;
};

}
