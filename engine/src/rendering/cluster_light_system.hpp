#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"
#include "vulkan/ve_descriptors.hpp"

#include <memory>
#include <array>
#include <filesystem>

namespace ve {
	class VeDevice;
	class VeBuffer;
	class VeComputePipeline;
	class EventBus;
}

namespace ve {

// Cluster parameters (UBO).
// Must match the ClusterParams struct in ve_cluster.slangh.
struct ClusterParams {
	alignas(16) glm::mat4 inv_proj{1.0f};
	alignas(16) glm::mat4 view{1.0f};
	alignas(4)  float z_near = 0.1f;
	alignas(4)  float z_far = 1000.0f;
	alignas(4)  float z_slice_scale = 1.0f;      // grid_dims.z / log(z_far / z_near)
	alignas(4)  uint32_t num_lights = 0;
	alignas(8)  glm::uvec2 screen_size{};
	alignas(8)  glm::uvec2 tile_size{};           // pixels per tile
	alignas(16) glm::uvec4 grid_dims{};           // xyz = (tiles_x, tiles_y, z_slices), w = total clusters
	alignas(4)  uint32_t cluster_enabled = 0;     // 0 = shader skips punctual+area lighting
	alignas(4)  uint32_t max_lights_per_cluster = ve::MAX_LIGHTS_PER_CLUSTER;
	alignas(4)  uint32_t num_point_lights = 0;    // num_lights[0, num_point) are point lights
	alignas(4)  uint32_t num_spot_lights = 0;     // num_lights[num_point, num_point+num_spot) are spot; rest are area
};

class VENGINE_API ClusterLightSystem {
public:
	ClusterLightSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		std::filesystem::path shader_path,
		vk::Extent2D screen_extent,
		EventBus& event_bus);
	~ClusterLightSystem();

	ClusterLightSystem(const ClusterLightSystem&) = delete;
	ClusterLightSystem& operator=(const ClusterLightSystem&) = delete;

	// Upload point light data from the ECS registry into the light SSBO.
	// Must be called after LightSystem::updateUniformBuffer() so light order matches.
	// Returns the number of lights uploaded.
	uint32_t uploadLightData(VeFrameInfo& frame_info);

	// Record the cluster assignment compute dispatch on the compute command buffer.
	void dispatch(VeFrameInfo& frame_info, vk::Extent2D screen_extent);

	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D screen_extent);

	void setLightCountActive(bool has_lights);
	bool isEnabled() const { return m_enabled; }

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
	uint32_t m_last_light_count = 0;
	uint32_t m_last_point_light_count = 0;
	uint32_t m_last_spot_light_count = 0;

	// Grid dimensions
	uint32_t m_tiles_x = 0;
	uint32_t m_tiles_y = 0;
	uint32_t m_total_clusters = 0;

	// Per-frame SSBOs
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_light_staging_ssbos;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_light_ssbos;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_cluster_count_ssbos;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_light_index_ssbos;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_cluster_param_ubos;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_spot_cone_ssbos;

	// Descriptor set layouts
	std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;
	std::unique_ptr<VeDescriptorSetLayout> m_output_set_layout;

	// Pipeline
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;

	// Per-frame descriptor sets
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_compute_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_output_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();
};

} // namespace ve
