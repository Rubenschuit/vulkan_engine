#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/meshlet_data.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "vulkan/ve_descriptors.hpp"

#include <glm/glm.hpp>
#include <array>
#include <filesystem>
#include <memory>

namespace ve {

class VeDevice;
class VeCamera;
class VeImage;
class GpuSceneManager;
class PbrMegaBuffer;
class HizSystem;
struct VeFrameInfo;

class VENGINE_API MeshletCullingSystem {
public:
	MeshletCullingSystem(VeDevice& device, const std::filesystem::path& shaders_dir);
	~MeshletCullingSystem();

	MeshletCullingSystem(const MeshletCullingSystem&) = delete;
	MeshletCullingSystem& operator=(const MeshletCullingSystem&) = delete;

	// Build frustum-only descriptor sets (call before HizSystem exists).
	void createDescriptorSets(VeDescriptorPool& pool,
	                          GpuSceneManager& scene_mgr,
	                          const PbrMegaBuffer& mega_buffer);

	// Recreate descriptor sets with real Hi-Z bound (call after HizSystem init and on resize).
	void createHizDescriptorSets(VeDescriptorPool& pool,
	                             GpuSceneManager& scene_mgr,
	                             const PbrMegaBuffer& mega_buffer,
	                             HizSystem& hiz);

	// Create global descriptor sets (set 0 in vertex shaders) bound to this system's instance buffer.
	void createGlobalDescriptorSets(VeDescriptorPool& pool,
	                                VeDescriptorSetLayout& global_layout,
	                                std::vector<std::unique_ptr<VeBuffer>>& ubo_buffers,
	                                VeBuffer& material_ssbo);

	// Dispatch two-pass compute (pass1 obj cull + indirection map, pass2 meshlet cull).
	void dispatch(vk::raii::CommandBuffer& cmd, VeFrameInfo& frame_info, GpuSceneManager& scene_mgr);

	// Shadow things
	static constexpr uint32_t SHADOW_BUFFER_COUNT = NUM_CSM_CASCADES + MAX_SHADOW_LIGHTS;

	// Build shadow descriptor sets (dummy Hi-Z; call before HizSystem exists).
	void createShadowDescriptorSets(VeDescriptorPool& pool,
	                                GpuSceneManager& scene_mgr,
	                                const PbrMegaBuffer& mega_buffer);

	// Recreate shadow descriptor sets with real Hi-Z bound (call after HizSystem init and on resize).
	void createShadowHizDescriptorSets(VeDescriptorPool& pool,
	                                   GpuSceneManager& scene_mgr,
	                                   const PbrMegaBuffer& mega_buffer,
	                                   HizSystem& hiz);

	// Create shadow global descriptor sets (set 0 for shadow vertex shaders).
	void createShadowGlobalDescriptorSets(VeDescriptorPool& pool,
	                                      VeDescriptorSetLayout& layout,
	                                      std::vector<std::vector<std::unique_ptr<VeBuffer>>>& csm_ubos,
	                                      std::vector<std::vector<std::unique_ptr<VeBuffer>>>& shadow_ubos);

	struct ShadowCullRequest {
		glm::mat4 view_proj;
		glm::vec3 light_pos;
		uint32_t slot;
		int32_t lod_bias;
	};

	// Batched 2-pass meshlet shadow cull for all shadow layers at once.
	void dispatchShadowCulls(vk::raii::CommandBuffer& cmd,
	                         const ShadowCullRequest* requests, uint32_t count,
	                         GpuSceneManager& scene_mgr,
	                         uint32_t frame_index,
	                         const VeCamera* camera = nullptr);

	VeBuffer& getShadowMeshletIndirectBuffer(uint32_t frame, uint32_t slot) {
		return *m_shadow_meshlet_indirect[frame][slot];
	}
	VeBuffer& getShadowMeshletDrawCounts(uint32_t frame, uint32_t slot) {
		return *m_shadow_meshlet_draw_counts[frame][slot];
	}
	VeBuffer& getShadowInstanceBuffer(uint32_t frame, uint32_t slot) {
		return *m_shadow_instance_buffers[frame][slot];
	}
	vk::raii::DescriptorSet& getShadowGlobalDescriptorSet(uint32_t frame, uint32_t slot) {
		return m_shadow_global_sets[frame][slot];
	}

	VeBuffer& getInstanceBuffer(uint32_t frame) { return *m_instance_buffers[frame]; }
	VeBuffer& getMeshletIndirectBuffer(uint32_t frame) { return *m_meshlet_indirect[frame]; }
	VeBuffer& getMeshletDrawCounts(uint32_t frame) { return *m_meshlet_draw_counts[frame]; }
	vk::raii::DescriptorSet& getGlobalDescriptorSet(uint32_t frame) { return m_global_descriptor_sets[frame]; }

	// CPU-side draw counts read back from 2 frames ago. Valid after the first dispatch cycle.
	// Returns nullptr when drawIndirectCount is supported (readback not needed).
	const uint32_t* getCpuDrawCounts() const { return m_has_readback ? m_readback_counts.data() : nullptr; }

	bool isHizEnabled() const { return m_hiz_enabled; }
	void setHizEnabled(bool enabled) { m_hiz_enabled = enabled; }

	static constexpr uint32_t BUCKET_COUNT = MESHLET_BUCKET_COUNT;

private:
	void createPipelineLayouts();

	VeDevice& m_ve_device;
	bool m_hiz_enabled = false;
	glm::vec2 m_hiz_size{0.0f};
	uint32_t m_hiz_mip_count = 0;

	// Per-frame camera view matrices for Hi-Z reprojection.
	std::array<glm::mat4, MAX_FRAMES_IN_FLIGHT> m_frame_views{glm::mat4{1.0f}, glm::mat4{1.0f}};

	// Descriptor set layouts
	std::unique_ptr<VeDescriptorSetLayout> m_pass1_layout;
	std::unique_ptr<VeDescriptorSetLayout> m_pass2_layout;

	// Pipeline layouts
	vk::raii::PipelineLayout m_pass1_pipeline_layout{nullptr};
	vk::raii::PipelineLayout m_pass2_pipeline_layout{nullptr};

	// Compute pipelines
	std::unique_ptr<VeComputePipeline> m_pass1_pipeline;
	std::unique_ptr<VeComputePipeline> m_pass2_pipeline;

	// Pass 1 descriptor sets (frustum-only and Hi-Z)
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_pass1_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_pass1_hiz_sets =
		makeNullArray<vk::raii::DescriptorSet>();

	// Pass 2 descriptor sets (frustum-only and Hi-Z)
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_pass2_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_pass2_hiz_sets =
		makeNullArray<vk::raii::DescriptorSet>();

	// Global descriptor sets (set 0 for rendering - binding 1 = this system's instance buffer)
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_global_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();

	// Per-frame compute buffers
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_visible_objects;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_meshlet_object_map;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_counts;           // [0]=visible_count, [1]=total_meshlets
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_dispatch_indirect;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_meshlet_indirect;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_meshlet_draw_counts;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_instance_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_cull_param_ubos;

	// Dummy 1x1 image + sampler for frustum-only path placeholders
	std::unique_ptr<VeImage> m_dummy_image;
	vk::raii::Sampler m_dummy_sampler{nullptr};

	// Async readback of per-bucket draw counts (fallback when drawIndirectCount unavailable)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_readback_staging;
	std::array<uint32_t, MESHLET_BUCKET_COUNT> m_readback_counts{};
	bool m_has_readback = false;

	// Shadow culling: per-shadow-layer buffers [frame][slot]
	using ShadowBufSet = std::array<std::unique_ptr<VeBuffer>, SHADOW_BUFFER_COUNT>;

	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_visible_objects;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_meshlet_object_map;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_counts;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_dispatch_indirect;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_meshlet_indirect;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_meshlet_draw_counts;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_instance_buffers;
	std::array<ShadowBufSet, MAX_FRAMES_IN_FLIGHT> m_shadow_cull_param_ubos;

	// Shadow compute descriptor sets: [frame][slot]
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_pass1_sets;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_pass2_sets;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_pass1_hiz_sets;
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_pass2_hiz_sets;

	// Shadow global descriptor sets for vertex shader: [frame][slot]
	std::vector<std::vector<vk::raii::DescriptorSet>> m_shadow_global_sets;
};

} // namespace ve
