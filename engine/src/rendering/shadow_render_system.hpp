/*
 * ShadowRenderSystem: Renders shadow maps into a unified 2D depth atlas.
 *
 * Supports three rendering paths:
 *   - CPU-driven:  frustum-culled draw list built on the CPU, instanced draws
 *   - GPU-culled:  per-light compute culling via GpuCullingSystem, indirect draws
 *   - Meshlet:     two-pass meshlet culling via MeshletCullingSystem, indirect draws
 *
 * The atlas holds CSM cascades (with incremental scroll-based updates) and
 * point/spot light shadow maps packed together.
 */

#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "ve_tracy.hpp"
#include "rendering/ve_frame_info.hpp"
#include "vulkan/ve_descriptors.hpp"

#include <memory>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <functional>
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
	class EventBus;
}

namespace ve {

struct ShadowPassUBO {
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
	alignas(16) glm::mat4 projection_view;
};
static_assert(sizeof(ShadowPassUBO) == 192, "ShadowPassUBO must be 192 bytes");

class VENGINE_API ShadowRenderSystem {
public:
	ShadowRenderSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& material_set_layout,
		std::filesystem::path shader_path,
		EventBus& event_bus);
	~ShadowRenderSystem();

	ShadowRenderSystem(const ShadowRenderSystem&) = delete;
	ShadowRenderSystem& operator=(const ShadowRenderSystem&) = delete;

	// --- UBO update ---

	void updateUniformBuffer(uint32_t frame_index, UniformBufferObject& ubo,
	                         const CsmCascadeData& csm_data);

	// --- Shadow map rendering (one of three paths is used per frame) ---

	void renderShadowMaps(VeFrameInfo& frame_info, PbrMegaBuffer& mega_buffer);

	void createGpuShadowDescriptorSets(GpuCullingSystem& gpu_cull);
	void renderShadowMapsGpuCulled(VeFrameInfo& frame_info,
	                               GpuCullingSystem& gpu_cull,
	                               PbrMegaBuffer& mega_buffer,
	                               GpuSceneManager& scene_mgr);

	void createMeshletShadowDescriptorSets(MeshletCullingSystem& meshlet_cull);
	void renderShadowMapsGpuCulledMeshlets(VeFrameInfo& frame_info,
	                                       MeshletCullingSystem& meshlet_cull,
	                                       PbrMegaBuffer& mega_buffer,
	                                       GpuSceneManager& scene_mgr);

	void releaseGpuShadowDescriptorSets();
	void releaseMeshletShadowDescriptorSets();

	// Optional
	void setTracyContext(TracyVkCtx ctx) { m_tracy_gfx_ctx = ctx; }

	// --- Accessors ---

	vk::raii::DescriptorSet& getShadowDescriptorSet(uint32_t frame_index) {
		return m_shadow_descriptor_sets[frame_index];
	}

	const vk::raii::DescriptorSetLayout& getShadowSetLayout() const {
		return m_shadow_set_layout->getDescriptorSetLayout();
	}

	// Used by LightSystem for atlas bias matrix computation
	const std::array<FrameAtlasRegion, MAX_SHADOW_LAYERS>& getAtlasRegions() const { return m_atlas_regions; }
	uint32_t getAtlasWidth() const { return m_atlas_width; }
	uint32_t getAtlasHeight() const { return m_atlas_height; }
	const VeImage* getAtlasImage() const { return m_shadow_atlas.get(); }
	const vk::raii::Sampler& getRawSampler() const { return m_shadow_raw_sampler; }

	uint32_t getCsmCascadeResolution(uint32_t cascade) const { return m_csm_cascade_resolutions[cascade]; }
	const uint32_t* getCsmCascadeResolutions() const { return m_csm_cascade_resolutions.data(); }
	ShadowResolutionPreset getResolutionPreset() const { return m_resolution_preset; }

	// --- Cache management ---

	void invalidateShadowDrawables();
	void forceShadowRerender();
	void subscribeToRegistry(Registry& registry);

private:
	// --- Internal types ---

	struct ShadowDrawable {
		Entity entity;
		MeshComponent* mesh = nullptr;
		uint32_t lod_level = 0;
	};

	struct SkinnedShadowDrawable {
		Entity entity;
		VeMesh* mesh = nullptr;
		uint32_t instance_offset = 0;
	};

	struct ShadowInstanceGroup {
		VeMesh* mesh = nullptr;
		uint32_t lod_level = 0;
		uint32_t first_instance = 0;
		uint32_t instance_count = 0;
	};

	struct StripRegion {
		uint32_t x, y, width, height;
	};

	// --- Initialization ---

	void createShadowResources();
	void resizeShadowAtlas(ShadowResolutionPreset preset, VeDescriptorPool& descriptor_pool);
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& material_set_layout);
	void createPipeline(vk::Format depth_format);
	void computeAtlasLayout();
	void createShadowUBOs();
	void createShadowInstanceBuffers();
	void createShadowPassDescriptorSets(VeDescriptorPool& descriptor_pool);
	void createShadowTextureDescriptorSets(VeDescriptorPool& descriptor_pool);
	void ensureCascadeUbos();
	void createShadowSlotDescriptorSets(
		std::function<VeBuffer&(uint32_t frame, uint32_t slot)> get_instance_buffer,
		std::vector<std::vector<vk::raii::DescriptorSet>>& out_cascade_sets,
		std::vector<std::vector<vk::raii::DescriptorSet>>& out_cascade_dynamic_sets,
		std::vector<std::vector<vk::raii::DescriptorSet>>& out_light_sets);

	// --- Rendering helpers ---

	void transitionAtlasForRendering(vk::raii::CommandBuffer& cmd, uint32_t csm_count);
	void transitionAtlasPostRender(vk::raii::CommandBuffer& cmd);
	void beginShadowRegionRender(
		vk::raii::CommandBuffer& cmd,
		const FrameAtlasRegion& region,
		vk::AttachmentLoadOp load_op,
		const StripRegion* strip_clear = nullptr);
	void renderShadowMap(VeFrameInfo& frame_info, uint32_t light_index,
		const std::vector<ShadowInstanceGroup>& instance_groups,
		const PbrMegaBuffer& mega_buffer,
		bool include_skinned = false) const;
	void growShadowInstanceBuffers(uint32_t new_capacity);

	// --- CSM incremental scroll ---

	std::vector<StripRegion> computeStripRegions(uint32_t cascade) const;
	glm::mat4 computeStripFrustumVP(uint32_t cascade, const StripRegion& strip, uint32_t frame) const;
	void copyPreservedRegion(vk::raii::CommandBuffer& cmd, uint32_t cascade);
	void updateCascadeCache(vk::raii::CommandBuffer& cmd, uint32_t cascade);

	// --- Static/dynamic shadow helpers ---

	// Delegates to GpuSceneManager::isDynamicEntity
	static bool isDynamicEntity(const Registry& registry, Entity entity);
	void copyStaticCacheToAtlas(vk::raii::CommandBuffer& cmd, uint32_t cascade);
	void snapshotAtlasToStaticCache(vk::raii::CommandBuffer& cmd, uint32_t cascade);

	// --- Deferred buffer deletion ---

	void retireBuffer(std::unique_ptr<VeBuffer> buffer);
	void flushRetiredBuffers();

	// --- Members ---

	VeDevice& m_ve_device;
	VeDescriptorPool& m_descriptor_pool;
	std::filesystem::path m_shader_path;
	Registry* m_registry = nullptr;

	std::unique_ptr<VeDescriptorSetLayout> m_shadow_global_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;

	std::vector<std::vector<std::unique_ptr<VeBuffer>>> m_shadow_ubos;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_global_descriptor_sets;
	std::vector<std::vector<glm::mat4>> m_light_views;
	std::vector<std::vector<glm::mat4>> m_light_projs;

	std::array<FrameAtlasRegion, MAX_SHADOW_LAYERS> m_atlas_regions{};
	uint32_t m_atlas_width = 0;
	uint32_t m_atlas_height = 0;
	ShadowResolutionPreset m_resolution_preset = ShadowResolutionPreset::MEDIUM;
	std::array<uint32_t, NUM_CSM_CASCADES> m_csm_cascade_resolutions{
		CSM_CASCADE_RESOLUTIONS[0], CSM_CASCADE_RESOLUTIONS[1], CSM_CASCADE_RESOLUTIONS[2]};
	uint32_t m_point_shadow_resolution = POINT_SHADOW_RESOLUTION;
	uint32_t m_spot_shadow_resolution = SPOT_SHADOW_RESOLUTION;
	std::unique_ptr<VeImage> m_shadow_atlas;
	vk::raii::Sampler m_shadow_sampler{nullptr};
	vk::raii::Sampler m_shadow_raw_sampler{nullptr};
	std::unique_ptr<VeDescriptorSetLayout> m_shadow_set_layout;
	std::vector<vk::raii::DescriptorSet> m_shadow_descriptor_sets;
	vk::Format m_shadow_depth_format;

	static constexpr uint32_t INITIAL_SHADOW_INSTANCE_CAPACITY = 16384;
	uint32_t m_shadow_instance_capacity = INITIAL_SHADOW_INSTANCE_CAPACITY;
	std::vector<std::unique_ptr<VeBuffer>> m_shadow_instance_buffers;

	std::vector<ShadowDrawable> m_static_shadow_drawables;
	std::vector<ShadowDrawable> m_dynamic_shadow_drawables;
	std::vector<SkinnedShadowDrawable> m_deformed_shadow_drawables;
	bool m_static_drawables_dirty = true;
	bool m_dynamic_drawables_dirty = true;
	std::vector<ShadowInstanceGroup> m_shadow_instance_groups;       // point/spot (all objects)
	std::vector<ShadowInstanceGroup> m_static_csm_instance_groups;
	std::vector<ShadowInstanceGroup> m_dynamic_csm_instance_groups;

	// Per-cascade scroll tracking for incremental CSM updates
	struct CascadeScrollState {
		glm::mat4 prev_view{0.0f};
		glm::mat4 prev_proj{0.0f};
		bool valid = false;
		bool dirty = true;

		glm::vec3 prev_center{0.0f};
		float prev_radius = 0.0f;
		float current_radius = 0.0f;
		glm::ivec2 texel_shift{0, 0};
		bool incremental = false;
	};
	std::array<CascadeScrollState, NUM_CSM_CASCADES> m_cascade_state;
	bool m_force_full_rerender = true;
	// Scratch images for the CSM scroll copy sequence (one per cascade).
	// Each incremental update snapshots the atlas cascade region into the cache,
	// then shift-copies it back.
	std::array<std::unique_ptr<VeImage>, NUM_CSM_CASCADES> m_cascade_cache;

	std::vector<std::vector<vk::raii::DescriptorSet>> m_gpu_cascade_descriptor_sets;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_gpu_cascade_dynamic_descriptor_sets;
	std::vector<std::vector<std::unique_ptr<VeBuffer>>> m_csm_cascade_ubos;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_gpu_shadow_descriptor_sets;

	std::vector<std::vector<vk::raii::DescriptorSet>> m_meshlet_cascade_descriptor_sets;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_meshlet_cascade_dynamic_descriptor_sets;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_meshlet_shadow_descriptor_sets;

	struct RetiredBuffer {
		std::unique_ptr<VeBuffer> buffer;
		uint32_t frames_remaining;
	};
	std::vector<RetiredBuffer> m_retired_buffers;

	TracyVkCtx m_tracy_gfx_ctx = nullptr;
};

}