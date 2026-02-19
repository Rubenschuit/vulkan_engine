#include "pch.hpp"
#include "rendering/cluster_light_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "utils/ve_log.hpp"

#include <cmath>

namespace ve {

ClusterLightSystem::ClusterLightSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	std::filesystem::path shader_path,
	vk::Extent2D screen_extent)
	: m_ve_device(device), m_shader_path(std::move(shader_path)) {

	createBuffers(screen_extent);
	createComputeSetLayout();
	createOutputSetLayout();
	createPipelineLayout(global_set_layout);
	createPipeline();
	createDescriptorSets(descriptor_pool);
}

ClusterLightSystem::~ClusterLightSystem() = default;

void ClusterLightSystem::createBuffers(vk::Extent2D screen_extent) {
	m_tiles_x = (screen_extent.width + CLUSTER_TILE_SIZE - 1) / CLUSTER_TILE_SIZE;
	m_tiles_y = (screen_extent.height + CLUSTER_TILE_SIZE - 1) / CLUSTER_TILE_SIZE;
	m_total_clusters = m_tiles_x * m_tiles_y * CLUSTER_Z_SLICES;

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_light_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(PointLight),
			MAX_CLUSTER_LIGHTS,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_light_ssbos[i]->map();

		m_cluster_count_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(glm::uvec2),
			m_total_clusters,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_light_index_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(uint32_t),
			m_total_clusters * MAX_LIGHTS_PER_CLUSTER,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_atomic_counter_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(uint32_t),
			1,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_cluster_param_ubos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(ClusterParams),
			1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment);
		m_cluster_param_ubos[i]->map();
	}
}

void ClusterLightSystem::createComputeSetLayout() {
	m_compute_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // light data
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // cluster counts
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // light index list
		.addBinding(3, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute)  // cluster params
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // atomic counter
		.build();
}

void ClusterLightSystem::createOutputSetLayout() {
	m_output_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eFragment)  // light data
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eFragment)  // cluster counts
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eFragment)  // light index list
		.addBinding(3, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eFragment)  // cluster params
		.build();
}

void ClusterLightSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout) {
	std::array<vk::DescriptorSetLayout, 2> set_layouts{
		*global_set_layout,
		*m_compute_set_layout->getDescriptorSetLayout(),
	};

	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void ClusterLightSystem::createPipeline() {
	m_compute_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path, m_pipeline_layout);
}

void ClusterLightSystem::createDescriptorSets(VeDescriptorPool& descriptor_pool) {
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto light_info = m_light_ssbos[i]->getDescriptorInfo();
		auto count_info = m_cluster_count_ssbos[i]->getDescriptorInfo();
		auto index_info = m_light_index_ssbos[i]->getDescriptorInfo();
		auto param_info = m_cluster_param_ubos[i]->getDescriptorInfo();
		auto atomic_info = m_atomic_counter_ssbos[i]->getDescriptorInfo();

		VeDescriptorWriter(*m_compute_set_layout, descriptor_pool)
			.writeBuffer(0, &light_info)
			.writeBuffer(1, &count_info)
			.writeBuffer(2, &index_info)
			.writeBuffer(3, &param_info)
			.writeBuffer(4, &atomic_info)
			.build(m_compute_descriptor_sets[i]);

		VeDescriptorWriter(*m_output_set_layout, descriptor_pool)
			.writeBuffer(0, &light_info)
			.writeBuffer(1, &count_info)
			.writeBuffer(2, &index_info)
			.writeBuffer(3, &param_info)
			.build(m_output_descriptor_sets[i]);
	}
}

uint32_t ClusterLightSystem::uploadLightData(VeFrameInfo& frame_info) {
	uint32_t frame = frame_info.current_frame;

	// Write cluster_enabled=0 by default; dispatch() will overwrite with 1 if called.
	// This ensures the shader takes the brute-force path when clustering is off.
	ClusterParams disabled_params{};
	m_cluster_param_ubos[frame]->writeToBuffer(&disabled_params);

	auto* buffer = static_cast<PointLight*>(m_light_ssbos[frame]->getMappedMemory());
	uint32_t count = 0;

	auto& registry = *frame_info.registry;
	auto& pl_pool = registry.pointLights();

	// Must iterate in same order as LightSystem::updateUniformBuffer()
	// so light indices match for shadow lookups.
	for (uint32_t i = 0; i < pl_pool.size() && count < MAX_CLUSTER_LIGHTS; i++) {
		uint32_t entity_idx = pl_pool.entityAt(i);
		Entity entity = registry.entityFromIndex(entity_idx);
		if (!registry.isActive(entity)) continue;
		auto* transform = registry.getComponent<TransformComponent>(entity);
		if (!transform) continue;

		PointLightComponent& pl = pl_pool.data()[i];
		glm::vec3 color = pl.getColor();
		float intensity = pl.getIntensity();
		buffer[count].position = glm::vec4{transform->getTranslation(), pl.getEffectiveRange()};
		buffer[count].color.x = color.x * intensity;
		buffer[count].color.y = color.y * intensity;
		buffer[count].color.z = color.z * intensity;
		buffer[count].color.w = intensity;
		count++;
	}

	m_last_light_count = count;
	return count;
}

void ClusterLightSystem::dispatch(VeFrameInfo& frame_info, const VeCamera& camera, vk::Extent2D screen_extent) {
	uint32_t frame = frame_info.current_frame;
	auto& cmd = frame_info.compute_command_buffer;

	// Update cluster params UBO
	ClusterParams params{};
	params.inv_proj = glm::inverse(camera.getProj());
	params.view = camera.getView();
	params.z_near = camera.getNear();
	params.z_far = camera.getFar();
	params.log_depth_ratio = std::log(params.z_far / params.z_near);
	params.num_lights = m_last_light_count;
	params.screen_size = glm::uvec2(screen_extent.width, screen_extent.height);
	params.tile_size = glm::uvec2(CLUSTER_TILE_SIZE, CLUSTER_TILE_SIZE);
	params.grid_dims = glm::uvec4(m_tiles_x, m_tiles_y, CLUSTER_Z_SLICES, m_total_clusters);
	params.cluster_enabled = 1;
	params.max_lights_per_cluster = MAX_LIGHTS_PER_CLUSTER;
	m_cluster_param_ubos[frame]->writeToBuffer(&params);

	// Reset atomic counter and cluster counts to zero
	cmd.fillBuffer(*m_atomic_counter_ssbos[frame]->getBuffer(), 0, sizeof(uint32_t), 0);
	cmd.fillBuffer(*m_cluster_count_ssbos[frame]->getBuffer(), 0,
		static_cast<vk::DeviceSize>(m_total_clusters) * sizeof(glm::uvec2), 0);

	// Barrier: fillBuffer → compute shader
	vk::MemoryBarrier2 fill_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
		.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
	};
	vk::DependencyInfo fill_dep{
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &fill_barrier,
	};
	cmd.pipelineBarrier2(fill_dep);

	// Bind pipeline and descriptor sets
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	std::array<vk::DescriptorSet, 2> sets{
		*frame_info.global_descriptor_set,
		*m_compute_descriptor_sets[frame],
	};
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout, 0, sets, {});

	// Dispatch: workgroup size = 256, flat over XY clusters per Z-slice
	uint32_t clusters_per_slice = m_tiles_x * m_tiles_y;
	constexpr uint32_t WORKGROUP_SIZE = 256;
	uint32_t groups_xy = (clusters_per_slice + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
	cmd.dispatch(groups_xy, 1, CLUSTER_Z_SLICES);

	// Barrier: compute write → fragment read
	vk::MemoryBarrier2 compute_to_frag{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
	};
	vk::DependencyInfo compute_dep{
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &compute_to_frag,
	};
	cmd.pipelineBarrier2(compute_dep);
}

void ClusterLightSystem::recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D screen_extent) {
	m_ve_device.getDevice().waitIdle();
	createBuffers(screen_extent);
	createDescriptorSets(descriptor_pool);
}

} // namespace ve
