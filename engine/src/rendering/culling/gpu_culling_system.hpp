#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "events/event_bus.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "vulkan/ve_descriptors.hpp"

#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <vector>

namespace ve {

class VeDevice;
class VeImage;
class GpuSceneManager;
class HizSystem;
struct VeFrameInfo;
struct CameraView;

struct CullParams {
	alignas(16) glm::vec4 frustum_planes[6];
	alignas(16) glm::mat4 view_proj;
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 prev_view{1.0f};
	alignas(4) float p00;              // projection[0][0] (horizontal focal length)
	alignas(4) float p11;              // projection[1][1] (vertical focal length)
	alignas(4) float p22;              // projection[2][2] (depth mapping)
	alignas(4) float p32;              // projection[3][2] (depth offset)
	alignas(4) uint32_t object_count;
	alignas(4) uint32_t hiz_enabled;   // 0 = frustum only, 1 = frustum + prev-frame Hi-Z
	alignas(4) uint32_t is_shadow_pass;
	alignas(4) uint32_t hiz_mip_count;
	alignas(8) glm::vec2 hiz_size;     // screen (not padded) extent
	alignas(8) glm::vec2 hiz_uv_scale; // screen_size / hiz_pad_size
	alignas(4) int32_t  lod_bias;
	alignas(4) uint32_t max_meshlet_draws{MAX_MESHLET_DRAWS};
	alignas(16) glm::vec4 camera_pos;  // for cone culling
	alignas(4) uint32_t bucket_count{MESHLET_BUCKET_COUNT};
	alignas(4) uint32_t shadow_cone_cull{0};
};

class VENGINE_API GpuCullingSystem {
public:
	GpuCullingSystem(VeDevice& device, const std::filesystem::path& shaders_dir);
	~GpuCullingSystem();

	GpuCullingSystem(const GpuCullingSystem&) = delete;
	GpuCullingSystem& operator=(const GpuCullingSystem&) = delete;

	void createDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr);
	void createShadowDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr);
	void createShadowHizDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr,
	                                   HizSystem& hiz);
	void createHizDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr,
	                             HizSystem& hiz);

	void createGlobalDescriptorSets(VeDescriptorPool& pool,
	                                VeDescriptorSetLayout& global_layout,
	                                std::vector<std::unique_ptr<VeBuffer>>& ubo_buffers,
	                                VeBuffer& material_ssbo);

	void dispatch(vk::raii::CommandBuffer& cmd, VeFrameInfo& frame_info, GpuSceneManager& scene_mgr);

	// Dispatches shadow culling for one independent shadow buffer slot.
	// Call for every shadow layer (cascades 0..csm_count-1, then lights at NUM_CSM_CASCADES+i).
	// If camera_view is provided and Hi-Z is enabled, camera-space occlusion culling is applied.
	void dispatchShadowCull(vk::raii::CommandBuffer& cmd,
	                        const glm::mat4& light_view_proj,
	                        GpuSceneManager& scene_mgr,
	                        uint32_t frame_index,
	                        uint32_t shadow_buf_index,
	                        int32_t lod_bias,
	                        const CameraView* camera_view = nullptr,
	                        ShadowPassMode shadow_mode = ShadowPassMode::ALL_OBJECTS);

	// Single global compute->draw barrier covering all shadow buffer slots.
	// Call once after all dispatchShadowCull calls for a frame.
	// If compaction is enabled, also dispatches shadow compaction passes before the barrier.
	void flushShadowCullBarrier(vk::raii::CommandBuffer& cmd, GpuSceneManager& scene_mgr,
	                            uint32_t frame_index);

	VeBuffer& getIndirectBuffer(uint32_t frame) { return *m_indirect_buffers[frame]; }
	VeBuffer& getInstanceBuffer(uint32_t frame) { return *m_instance_buffers[frame]; }
	vk::raii::DescriptorSet& getGlobalDescriptorSet(uint32_t frame) { return m_global_descriptor_sets[frame]; }

	// Shadow culling output accessors (per slot; slot = array_layer = shadow_buf_index)
	VeBuffer& getShadowIndirectBuffer(uint32_t frame, uint32_t slot) { return *m_shadow_indirect_buffers[frame][slot]; }
	VeBuffer& getShadowInstanceBuffer(uint32_t frame, uint32_t slot) { return *m_shadow_instance_buffers[frame][slot]; }

	// Compaction output accessors (only valid when compactionEnabled())
	bool compactionEnabled() const { return m_compaction_enabled; }
	VeBuffer& getCompactedIndirectBuffer(uint32_t frame) { return *m_compacted_indirect_buffers[frame]; }
	VeBuffer& getCompactCountBuffer(uint32_t frame) { return *m_compact_count_buffers[frame]; }
	VeBuffer& getShadowCompactedIndirectBuffer(uint32_t frame, uint32_t slot) { return *m_shadow_compacted_indirect_buffers[frame][slot]; }
	VeBuffer& getShadowCompactCountBuffer(uint32_t frame, uint32_t slot) { return *m_shadow_compact_count_buffers[frame][slot]; }

	static constexpr uint32_t SHADOW_BUFFER_COUNT = NUM_CSM_CASCADES + MAX_SHADOW_LIGHTS;

	// Read back draw counts from a previous frame's staging buffer
	uint32_t readbackDrawCounts(uint32_t frame) const;
	// Exact triangle count accumulated by cull shader (index_count / 3)
	uint32_t readbackTriangleCount(uint32_t frame) const;
	// Raw per-bucket counts from readback
	const uint32_t* getReadbackCounts(uint32_t frame) const;
	// Zero readback buffers (call on scene change to avoid stale data).
	void clearReadback();

	bool isHizEnabled() const { return m_hiz_enabled; }
	void setHizEnabled(bool enabled) { m_hiz_enabled = enabled; }

	static constexpr uint32_t BUCKET_COUNT = GPU_CULL_BUCKET_COUNT;

	void createCompactionDescriptorSets(VeDescriptorPool& pool);

	void subscribeToEvents(EventBus& event_bus, HizSystem& hiz, GpuSceneManager& scene_mgr);

private:
	void createPipelineLayout();
	void createCompactionPipeline(const std::filesystem::path& shaders_dir);
	void dispatchCompaction(vk::raii::CommandBuffer& cmd, GpuSceneManager& scene_mgr, uint32_t frame);
	void dispatchShadowCompaction(vk::raii::CommandBuffer& cmd, GpuSceneManager& scene_mgr,
	                              uint32_t frame, uint32_t slot);

	void refreshMainIndirectBuffer(vk::raii::CommandBuffer& cmd, GpuSceneManager& scene_mgr,
	                               uint32_t frame);

	VeDevice& m_ve_device;
	bool m_hiz_enabled = false;
	bool m_compaction_enabled = false;
	glm::vec2 m_hiz_size{0.0f};
	glm::vec2 m_hiz_uv_scale{1.0f};
	uint32_t m_hiz_mip_count = 0;

	// Per-frame camera view matrices
	std::array<glm::mat4, MAX_FRAMES_IN_FLIGHT> m_frame_views{glm::mat4{1.0f}, glm::mat4{1.0f}};

	std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;

	// Frustum-only descriptor sets
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_compute_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();

	// Dummy image + sampler for binding 6-7 placeholders when Hi-Z is not active
	std::unique_ptr<VeImage> m_dummy_image;
	vk::raii::Sampler m_dummy_sampler{nullptr};
	// Reads prev-frame Hi-Z, outputs to main buffers
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_hiz_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();

	// Output buffers (device-local, written by compute, read by draw)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_indirect_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_draw_count_buffers;  // stats only
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_readback_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_instance_buffers;

	// Per-frame UBO for cull params
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_cull_param_ubos;

	// Global descriptor sets with GPU culling's instance buffer at binding 1
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_global_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();

	// Shadow culling
	using ShadowBufSet = std::array<std::unique_ptr<VeBuffer>, SHADOW_BUFFER_COUNT>;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_indirect_buffers;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_instance_buffers;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_cull_param_ubos;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_compute_descriptor_sets; // [frame][slot]

	// Draw command compaction (only when drawIndirectCount is supported)
	struct CompactPushConstants {
		uint32_t total_groups;
		uint32_t bucket_offsets[GPU_CULL_BUCKET_COUNT];
		uint32_t bucket_counts[GPU_CULL_BUCKET_COUNT];
	};
	std::unique_ptr<VeDescriptorSetLayout> m_compact_set_layout;
	vk::raii::PipelineLayout m_compact_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compact_pipeline;

	// Main pass
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_compacted_indirect_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_compact_count_buffers;
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_compact_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();
		
	// Shadow pass
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_compacted_indirect_buffers;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_compact_count_buffers;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_compact_descriptor_sets; // [frame][slot]

	EventBus* m_event_bus = nullptr;
	EventSubscriptionId m_resolution_sub = 0;
	EventSubscriptionId m_scene_unloaded_sub = 0;
};

}
