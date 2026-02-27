#include "pch.hpp"
#include "rendering/gpu_culling_system.hpp"
#include "rendering/gpu_scene_manager.hpp"
#include "rendering/ve_frame_info.hpp"
#include "scene/ve_camera.hpp"
#include "utils/ve_frustum.hpp"
#include "utils/ve_log.hpp"

namespace ve {

GpuCullingSystem::GpuCullingSystem(VeDevice& device)
	: m_ve_device(device) {

	constexpr uint32_t total_commands = BUCKET_COUNT * MAX_GPU_OBJECTS;

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		// Indirect command buffer (device-local, written by compute, consumed by draw)
		m_indirect_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(VkDrawIndexedIndirectCommand), total_commands,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
				| vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		// Draw count buffer: 4 uint32 (one per bucket)
		m_draw_count_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(uint32_t), BUCKET_COUNT,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
				| vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		// Readback staging buffer (host-visible, for async GPU->CPU draw count readback)
		m_readback_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(uint32_t), BUCKET_COUNT,
			vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_readback_buffers[i]->map();

		// Instance data buffer (device-local, written by compute, read by vertex shader)
		m_instance_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(InstanceData), total_commands,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		// Cull params UBO (host-visible, written by CPU each frame)
		m_cull_param_ubos[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(CullParams), 1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment);
		m_cull_param_ubos[i]->map();
	}

	// Descriptor set layout for compute
	m_compute_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // objects
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // transforms
		.addBinding(2, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute) // cull params
		.addBinding(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // indirect cmds
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // instance data
		.addBinding(5, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // draw counts
		.build();

	createPipelineLayout();

	m_compute_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, "shaders/gpu_cull_comp.spv", m_pipeline_layout);

	VE_LOGI("GpuCullingSystem: initialized (" << MAX_GPU_OBJECTS << " max commands per bucket, " << BUCKET_COUNT << " buckets)");
}

GpuCullingSystem::~GpuCullingSystem() = default;

void GpuCullingSystem::createPipelineLayout() {
	std::array<vk::DescriptorSetLayout, 1> set_layouts{
		*m_compute_set_layout->getDescriptorSetLayout(),
	};

	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void GpuCullingSystem::createDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr) {
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto obj_info = scene_mgr.getObjectDataBuffer(i).getDescriptorInfo();
		auto xform_info = scene_mgr.getTransformBuffer(i).getDescriptorInfo();
		auto params_info = m_cull_param_ubos[i]->getDescriptorInfo();
		auto indirect_info = m_indirect_buffers[i]->getDescriptorInfo();
		auto instance_info = m_instance_buffers[i]->getDescriptorInfo();
		auto count_info = m_draw_count_buffers[i]->getDescriptorInfo();

		VeDescriptorWriter(*m_compute_set_layout, pool)
			.writeBuffer(0, &obj_info)
			.writeBuffer(1, &xform_info)
			.writeBuffer(2, &params_info)
			.writeBuffer(3, &indirect_info)
			.writeBuffer(4, &instance_info)
			.writeBuffer(5, &count_info)
			.build(m_compute_descriptor_sets[i]);
	}
}

void GpuCullingSystem::createGlobalDescriptorSets(VeDescriptorPool& pool,
                                                   VeDescriptorSetLayout& global_layout,
                                                   std::vector<std::unique_ptr<VeBuffer>>& ubo_buffers,
                                                   VeBuffer& material_ssbo) {
	auto material_info = material_ssbo.getDescriptorInfo();
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto ubo_info = ubo_buffers[i]->getDescriptorInfo();
		auto instance_info = m_instance_buffers[i]->getDescriptorInfo();

		VeDescriptorWriter(global_layout, pool)
			.writeBuffer(0, &ubo_info)
			.writeBuffer(1, &instance_info)
			.writeBuffer(2, &material_info)
			.build(m_global_descriptor_sets[i]);
	}
}

void GpuCullingSystem::dispatch(VeFrameInfo& frame_info, const GpuSceneManager& scene_mgr) {
	uint32_t object_count = scene_mgr.getDispatchCount();
	if (object_count == 0)
		return;

	uint32_t frame = frame_info.current_frame;
	auto& cmd = frame_info.compute_command_buffer;

	// Update cull params UBO
	glm::mat4 vp = frame_info.camera.getProj() * frame_info.camera.getView();
	FrustumPlane cpu_planes[6];
	extractFrustumPlanes(vp, cpu_planes);

	CullParams params{};
	for (int i = 0; i < 6; i++)
		params.frustum_planes[i] = cpu_planes[i].plane;
	params.view_proj = vp;
	params.object_count = object_count;
	params.bucket_stride = MAX_GPU_OBJECTS;
	m_cull_param_ubos[frame]->writeToBuffer(&params);

	// Clear draw count buffer and indirect command buffer
	cmd.fillBuffer(*m_draw_count_buffers[frame]->getBuffer(), 0,
		static_cast<vk::DeviceSize>(BUCKET_COUNT) * sizeof(uint32_t), 0);
	cmd.fillBuffer(*m_indirect_buffers[frame]->getBuffer(), 0,
		static_cast<vk::DeviceSize>(BUCKET_COUNT * MAX_GPU_OBJECTS) * sizeof(VkDrawIndexedIndirectCommand), 0);

	// Barrier: transfer clear to compute read/write
	vk::MemoryBarrier2 clear_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
		.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
	};
	vk::DependencyInfo clear_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &clear_barrier};
	cmd.pipelineBarrier2(clear_dep);

	// Bind and dispatch
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout,
		0, {*m_compute_descriptor_sets[frame]}, {});

	uint32_t group_count = (object_count + GPU_CULL_WORKGROUP_SIZE - 1) / GPU_CULL_WORKGROUP_SIZE;
	cmd.dispatch(group_count, 1, 1);

	// Barrier: compute writes must complete before readback copy
	vk::MemoryBarrier2 copy_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
		.dstAccessMask = vk::AccessFlagBits2::eTransferRead,
	};
	vk::DependencyInfo copy_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &copy_barrier};
	cmd.pipelineBarrier2(copy_dep);

	// Copy draw counts to host-visible staging for async stats readback
	vk::BufferCopy count_copy{0, 0, static_cast<vk::DeviceSize>(BUCKET_COUNT) * sizeof(uint32_t)};
	cmd.copyBuffer(*m_draw_count_buffers[frame]->getBuffer(),
		*m_readback_buffers[frame]->getBuffer(), count_copy);

	// Cross-queue synchronization (compute writes -> graphics indirect reads / vertex shader reads)
	// is handled by the timeline semaphore in VeSwapChain::submitComputeWork / submitAndPresent.
}

uint32_t GpuCullingSystem::readbackDrawCounts(uint32_t frame) const {
	const auto* counts = static_cast<const uint32_t*>(m_readback_buffers[frame]->getMappedMemory());
	uint32_t total = 0;
	for (uint32_t b = 0; b < BUCKET_COUNT; b++)
		total += counts[b];
	return total;
}

} // namespace ve
