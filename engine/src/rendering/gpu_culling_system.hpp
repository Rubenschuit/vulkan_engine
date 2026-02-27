#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "vulkan/ve_descriptors.hpp"

#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <vector>

namespace ve {

class VeDevice;
class VeCamera;
class GpuSceneManager;
struct VeFrameInfo;

struct CullParams {
	alignas(16) glm::vec4 frustum_planes[6];
	alignas(16) glm::mat4 view_proj;
	alignas(4) uint32_t object_count;
	alignas(4) uint32_t bucket_stride; // MAX_GPU_OBJECTS per bucket
	alignas(4) uint32_t _pad0;
	alignas(4) uint32_t _pad1;
};

class VENGINE_API GpuCullingSystem {
public:
	GpuCullingSystem(VeDevice& device);
	~GpuCullingSystem();

	GpuCullingSystem(const GpuCullingSystem&) = delete;
	GpuCullingSystem& operator=(const GpuCullingSystem&) = delete;

	// Build descriptor sets that reference scene manager's buffers.
	// Call after GpuSceneManager is constructed.
	void createDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr);

	// Build global descriptor sets for the GPU-culled render path.
	// These point to the GPU culling's instance buffer at binding 1.
	void createGlobalDescriptorSets(VeDescriptorPool& pool,
	                                VeDescriptorSetLayout& global_layout,
	                                std::vector<std::unique_ptr<VeBuffer>>& ubo_buffers,
	                                VeBuffer& material_ssbo);

	void dispatch(VeFrameInfo& frame_info, const GpuSceneManager& scene_mgr);

	VeBuffer& getIndirectBuffer(uint32_t frame) { return *m_indirect_buffers[frame]; }
	VeBuffer& getCountBuffer(uint32_t frame) { return *m_draw_count_buffers[frame]; }
	VeBuffer& getInstanceBuffer(uint32_t frame) { return *m_instance_buffers[frame]; }
	vk::raii::DescriptorSet& getGlobalDescriptorSet(uint32_t frame) { return m_global_descriptor_sets[frame]; }

	// Read back draw counts from a previous frame's staging buffer (1 frame stale).
	uint32_t readbackDrawCounts(uint32_t frame) const;

	static constexpr uint32_t BUCKET_COUNT = GPU_CULL_BUCKET_COUNT;

	uint32_t getBucketOffset(uint32_t bucket) const { return bucket * MAX_GPU_OBJECTS; }

private:
	void createPipelineLayout();

	VeDevice& m_ve_device;

	// Compute pipeline
	std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_compute_descriptor_sets{
		vk::raii::DescriptorSet{nullptr}, vk::raii::DescriptorSet{nullptr}};

	// Output buffers (device-local, written by compute, read by draw)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_indirect_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_draw_count_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_readback_buffers;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_instance_buffers;

	// Per-frame UBO for cull params
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_cull_param_ubos;

	// Global descriptor sets with GPU culling's instance buffer at binding 1
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_global_descriptor_sets{
		vk::raii::DescriptorSet{nullptr}, vk::raii::DescriptorSet{nullptr}};
};

} // namespace ve
