#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"

#include <memory>
#include <array>
#include <filesystem>

namespace ve {
	class VeDevice;
	class VeBuffer;
	class VeDescriptorPool;
	class VeDescriptorSetLayout;
	class VeComputePipeline;
}

namespace ve {

class VENGINE_API ClusterLightSystem {
public:
	ClusterLightSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		std::filesystem::path shader_path,
		vk::Extent2D screen_extent);
	~ClusterLightSystem();

	ClusterLightSystem(const ClusterLightSystem&) = delete;
	ClusterLightSystem& operator=(const ClusterLightSystem&) = delete;

	/// Upload point light data from the ECS registry into the light SSBO.
	/// Must be called after LightSystem::updateUniformBuffer() so light order matches.
	/// Returns the number of lights uploaded.
	uint32_t uploadLightData(VeFrameInfo& frame_info);

	/// Record the cluster assignment compute dispatch on the compute command buffer.
	void dispatch(VeFrameInfo& frame_info, const VeCamera& camera, vk::Extent2D screen_extent);

	/// Recreate buffers when screen resolution changes.
	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D screen_extent);

	void setEnabled(bool enabled) { m_enabled = enabled; }
	bool isEnabled() const { return m_enabled; }

	/// Descriptor set layout for Set 4 (cluster data for PBR/simple fragment shaders)
	const vk::raii::DescriptorSetLayout& getOutputSetLayout() const {
		return m_output_set_layout->getDescriptorSetLayout();
	}

	/// Per-frame output descriptor set for fragment shader binding
	vk::raii::DescriptorSet& getOutputDescriptorSet(uint32_t frame_index) {
		return m_output_descriptor_sets[frame_index];
	}

private:
	void createBuffers(vk::Extent2D screen_extent);
	void createComputeSetLayout();
	void createOutputSetLayout();
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout);
	void createPipeline();
	void createDescriptorSets(VeDescriptorPool& descriptor_pool);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	bool m_enabled = false;
	uint32_t m_last_light_count = 0;  // set by uploadLightData(), read by dispatch()

	// Grid dimensions (recomputed on resize)
	uint32_t m_tiles_x = 0;
	uint32_t m_tiles_y = 0;
	uint32_t m_total_clusters = 0;

	// Per-frame SSBOs
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_light_ssbos;           // PointLight[]
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_cluster_count_ssbos;    // uvec2[] (offset, count)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_light_index_ssbos;      // uint[] (packed indices)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_atomic_counter_ssbos;   // uint (global index counter)
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_cluster_param_ubos;     // ClusterParams

	// Descriptor set layouts
	std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;  // Set 1 in compute pipeline
	std::unique_ptr<VeDescriptorSetLayout> m_output_set_layout;   // Set 4 in graphics pipeline

	// Pipeline
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;

	// Per-frame descriptor sets
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_compute_descriptor_sets{
		vk::raii::DescriptorSet{nullptr}, vk::raii::DescriptorSet{nullptr}};
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_output_descriptor_sets{
		vk::raii::DescriptorSet{nullptr}, vk::raii::DescriptorSet{nullptr}};
};

} // namespace ve
