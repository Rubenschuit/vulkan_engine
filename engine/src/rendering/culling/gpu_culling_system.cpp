#include "pch.hpp"
#include "rendering/culling/gpu_culling_system.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/ve_frame_info.hpp"
#include "vulkan/ve_image.hpp"
#include "scene/camera_view.hpp"
#include "utils/ve_frustum.hpp"
#include "utils/ve_log.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"

namespace ve {

GpuCullingSystem::GpuCullingSystem(VeDevice& device, const std::filesystem::path& shaders_dir)
	: m_ve_device(device) {

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		// Indirect command buffer: one command per draw group (not per object)
		m_indirect_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(VkDrawIndexedIndirectCommand), MAX_DRAW_GROUPS,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
				| vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		// Draw count buffer: per-bucket visible object count + total index count (stats only)
		m_draw_count_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(uint32_t), BUCKET_COUNT + 1,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst
				| vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		// Readback staging buffer (host-visible, for async GPU->CPU draw count readback)
		m_readback_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(uint32_t), BUCKET_COUNT + 1,
			vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_readback_buffers[i]->map();
		std::memset(m_readback_buffers[i]->getMappedMemory(), 0,
			(BUCKET_COUNT + 1) * sizeof(uint32_t));

		// Instance data buffer: one slot per (object × LOD level) worst-case
		m_instance_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(InstanceData), MAX_LOD_INSTANCE_SLOTS,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_cull_param_ubos[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(CullParams), 1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment);
		m_cull_param_ubos[i]->map();
	}

	m_compute_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // objects
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // transforms
		.addBinding(2, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute) // cull params
		.addBinding(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // indirect cmds (RW)
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // instance data
		.addBinding(5, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // draw counts (stats)
		.addBinding(6, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)  // hiz pyramid
		.addBinding(7, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eCompute)       // hiz sampler
		.addBinding(8, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // active IDs
		.addBinding(9, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // draw groups
		.build();

	createPipelineLayout();

	m_compute_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, shaders_dir / "gpu_cull_comp.spv", m_pipeline_layout);

	// Dummy 1x1 R32Float image + sampler for placeholder bindings 6-7 when Hi-Z is inactive
	m_dummy_image = std::make_unique<VeImage>(
		m_ve_device, 1, 1, vk::SampleCountFlagBits::e1,
		vk::Format::eR32Sfloat, vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1);
	m_dummy_image->transitionImageLayout(
		vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eComputeShader);
	m_dummy_image->setDebugName("GPU Cull Dummy Hi-Z");
	{
		vk::SamplerCreateInfo sampler_info{
			.magFilter = vk::Filter::eNearest,
			.minFilter = vk::Filter::eNearest,
			.mipmapMode = vk::SamplerMipmapMode::eNearest,
			.addressModeU = vk::SamplerAddressMode::eClampToEdge,
			.addressModeV = vk::SamplerAddressMode::eClampToEdge,
			.addressModeW = vk::SamplerAddressMode::eClampToEdge,
		};
		m_dummy_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
	}

	// Shadow culling output buffers: one independent set per shadow layer per frame
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		for (uint32_t slot = 0; slot < SHADOW_BUFFER_COUNT; slot++) {
			m_shadow_indirect_buffers[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(VkDrawIndexedIndirectCommand), MAX_DRAW_GROUPS,
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
					| vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_shadow_instance_buffers[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(InstanceData), MAX_LOD_INSTANCE_SLOTS,
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_shadow_cull_param_ubos[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(CullParams), 1,
				vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal,
				m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment);
		}
	}

	// Draw command compaction resources (only when drawIndirectCount is supported)
	m_compaction_enabled = m_ve_device.supportsDrawIndirectCount();
	if (m_compaction_enabled) {
		auto indirect_usage = vk::BufferUsageFlagBits::eStorageBuffer
			| vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst;
		auto count_usage = vk::BufferUsageFlagBits::eStorageBuffer
			| vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst;

		for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
			m_compacted_indirect_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(VkDrawIndexedIndirectCommand), MAX_DRAW_GROUPS,
				indirect_usage, vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_compact_count_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(uint32_t), BUCKET_COUNT,
				count_usage, vk::MemoryPropertyFlagBits::eDeviceLocal);

			for (uint32_t slot = 0; slot < SHADOW_BUFFER_COUNT; slot++) {
				m_shadow_compacted_indirect_buffers[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
					sizeof(VkDrawIndexedIndirectCommand), MAX_DRAW_GROUPS,
					indirect_usage, vk::MemoryPropertyFlagBits::eDeviceLocal);

				m_shadow_compact_count_buffers[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
					sizeof(uint32_t), BUCKET_COUNT,
					count_usage, vk::MemoryPropertyFlagBits::eDeviceLocal);
			}
		}
		createCompactionPipeline(shaders_dir);
	}

	VE_LOGI("GpuCullingSystem: initialized (" << MAX_DRAW_GROUPS << " max draw groups, "
		<< BUCKET_COUNT << " buckets, compaction=" << (m_compaction_enabled ? "on" : "off") << ")");
}

GpuCullingSystem::~GpuCullingSystem() {
	if (m_event_bus) {
		if (m_resolution_sub != 0)
			m_event_bus->unsubscribe<ResolutionChangedEvent>(m_resolution_sub);
		if (m_scene_unloaded_sub != 0)
			m_event_bus->unsubscribe<SceneUnloadedEvent>(m_scene_unloaded_sub);
	}
}

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

void GpuCullingSystem::createCompactionPipeline(const std::filesystem::path& shaders_dir) {
	m_compact_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // src indirect
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // dst compacted
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // count buffer
		.build();

	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(CompactPushConstants),
	};

	std::array<vk::DescriptorSetLayout, 1> set_layouts{
		*m_compact_set_layout->getDescriptorSetLayout(),
	};

	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};

	m_compact_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
	m_compact_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, shaders_dir / "gpu_compact_comp.spv", m_compact_pipeline_layout);
}

void GpuCullingSystem::createCompactionDescriptorSets(VeDescriptorPool& pool) {
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto src_info = m_indirect_buffers[i]->getDescriptorInfo();
		auto dst_info = m_compacted_indirect_buffers[i]->getDescriptorInfo();
		auto cnt_info = m_compact_count_buffers[i]->getDescriptorInfo();

		VeDescriptorWriter(*m_compact_set_layout, pool)
			.writeBuffer(0, &src_info)
			.writeBuffer(1, &dst_info)
			.writeBuffer(2, &cnt_info)
			.build(m_compact_descriptor_sets[i]);
	}

	m_shadow_compact_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_shadow_compact_descriptor_sets[i].clear();
		for (uint32_t slot = 0; slot < SHADOW_BUFFER_COUNT; slot++) {
			auto src_info = m_shadow_indirect_buffers[i][slot]->getDescriptorInfo();
			auto dst_info = m_shadow_compacted_indirect_buffers[i][slot]->getDescriptorInfo();
			auto cnt_info = m_shadow_compact_count_buffers[i][slot]->getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(*m_compact_set_layout, pool)
				.writeBuffer(0, &src_info)
				.writeBuffer(1, &dst_info)
				.writeBuffer(2, &cnt_info)
				.build(ds);
			m_shadow_compact_descriptor_sets[i].push_back(std::move(ds));
		}
	}
}

void GpuCullingSystem::createDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr) {
	vk::DescriptorImageInfo dummy_img{
		.imageView = *m_dummy_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo dummy_smp{.sampler = *m_dummy_sampler};

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto obj_info = scene_mgr.getObjectDataBuffer(i).getDescriptorInfo();
		auto xform_info = scene_mgr.getTransformBuffer(i).getDescriptorInfo();
		auto params_info = m_cull_param_ubos[i]->getDescriptorInfo();
		auto indirect_info = m_indirect_buffers[i]->getDescriptorInfo();
		auto instance_info = m_instance_buffers[i]->getDescriptorInfo();
		auto count_info = m_draw_count_buffers[i]->getDescriptorInfo();
		auto active_id_info = scene_mgr.getActiveIdBuffer(i).getDescriptorInfo();
		auto draw_group_info = scene_mgr.getDrawGroupBuffer(i).getDescriptorInfo();

		VeDescriptorWriter(*m_compute_set_layout, pool)
			.writeBuffer(0, &obj_info)
			.writeBuffer(1, &xform_info)
			.writeBuffer(2, &params_info)
			.writeBuffer(3, &indirect_info)
			.writeBuffer(4, &instance_info)
			.writeBuffer(5, &count_info)
			.writeImage(6, &dummy_img)
			.writeImage(7, &dummy_smp)
			.writeBuffer(8, &active_id_info)
			.writeBuffer(9, &draw_group_info)
			.build(m_compute_descriptor_sets[i]);
	}
}

void GpuCullingSystem::createShadowDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr) {
	vk::DescriptorImageInfo dummy_img{
		.imageView = *m_dummy_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo dummy_smp{.sampler = *m_dummy_sampler};

	m_shadow_compute_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto obj_info = scene_mgr.getObjectDataBuffer(i).getDescriptorInfo();
		auto xform_info = scene_mgr.getTransformBuffer(i).getDescriptorInfo();
		auto count_info = m_draw_count_buffers[i]->getDescriptorInfo();
		auto active_id_info = scene_mgr.getActiveIdBuffer(i).getDescriptorInfo();
		auto draw_group_info = scene_mgr.getDrawGroupBuffer(i).getDescriptorInfo();

		m_shadow_compute_descriptor_sets[i].clear();
		for (uint32_t slot = 0; slot < SHADOW_BUFFER_COUNT; slot++) {
			auto params_info = m_shadow_cull_param_ubos[i][slot]->getDescriptorInfo();
			auto indirect_info = m_shadow_indirect_buffers[i][slot]->getDescriptorInfo();
			auto instance_info = m_shadow_instance_buffers[i][slot]->getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(*m_compute_set_layout, pool)
				.writeBuffer(0, &obj_info)
				.writeBuffer(1, &xform_info)
				.writeBuffer(2, &params_info)
				.writeBuffer(3, &indirect_info)
				.writeBuffer(4, &instance_info)
				.writeBuffer(5, &count_info)
				.writeImage(6, &dummy_img)
				.writeImage(7, &dummy_smp)
				.writeBuffer(8, &active_id_info)
				.writeBuffer(9, &draw_group_info)
				.build(ds);
			m_shadow_compute_descriptor_sets[i].push_back(std::move(ds));
		}
	}
}

void GpuCullingSystem::createShadowHizDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr,
                                                      HizSystem& hiz) {
	m_shadow_compute_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto obj_info = scene_mgr.getObjectDataBuffer(i).getDescriptorInfo();
		auto xform_info = scene_mgr.getTransformBuffer(i).getDescriptorInfo();
		auto count_info = m_draw_count_buffers[i]->getDescriptorInfo();
		auto active_id_info = scene_mgr.getActiveIdBuffer(i).getDescriptorInfo();
		auto draw_group_info = scene_mgr.getDrawGroupBuffer(i).getDescriptorInfo();

		// Prev-frame Hi-Z (same image for all slots in a given frame)
		uint32_t prev_frame = (i + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
		vk::DescriptorImageInfo prev_hiz_info{
			.imageView = *hiz.getHizImageView(prev_frame),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo sampler_info{.sampler = *hiz.getSampler()};

		m_shadow_compute_descriptor_sets[i].clear();
		for (uint32_t slot = 0; slot < SHADOW_BUFFER_COUNT; slot++) {
			auto params_info = m_shadow_cull_param_ubos[i][slot]->getDescriptorInfo();
			auto indirect_info = m_shadow_indirect_buffers[i][slot]->getDescriptorInfo();
			auto instance_info = m_shadow_instance_buffers[i][slot]->getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(*m_compute_set_layout, pool)
				.writeBuffer(0, &obj_info)
				.writeBuffer(1, &xform_info)
				.writeBuffer(2, &params_info)
				.writeBuffer(3, &indirect_info)
				.writeBuffer(4, &instance_info)
				.writeBuffer(5, &count_info)
				.writeImage(6, &prev_hiz_info)
				.writeImage(7, &sampler_info)
				.writeBuffer(8, &active_id_info)
				.writeBuffer(9, &draw_group_info)
				.build(ds);
			m_shadow_compute_descriptor_sets[i].push_back(std::move(ds));
		}
	}
}

void GpuCullingSystem::createHizDescriptorSets(VeDescriptorPool& pool, GpuSceneManager& scene_mgr,
                                                HizSystem& hiz) {
	m_hiz_size = glm::vec2(static_cast<float>(hiz.getScreenWidth()),
	                        static_cast<float>(hiz.getScreenHeight()));
	m_hiz_uv_scale = glm::vec2(
		static_cast<float>(hiz.getScreenWidth())  / static_cast<float>(hiz.getWidth()),
		static_cast<float>(hiz.getScreenHeight()) / static_cast<float>(hiz.getHeight()));
	m_hiz_mip_count = hiz.getMipLevels();
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto obj_info = scene_mgr.getObjectDataBuffer(i).getDescriptorInfo();
		auto transform_info = scene_mgr.getTransformBuffer(i).getDescriptorInfo();
		auto active_id_info = scene_mgr.getActiveIdBuffer(i).getDescriptorInfo();
		auto draw_group_info = scene_mgr.getDrawGroupBuffer(i).getDescriptorInfo();

		// Prev-frame Hi-Z
		uint32_t prev_frame = (i + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
		vk::DescriptorImageInfo prev_hiz_info{
			.imageView = *hiz.getHizImageView(prev_frame),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo sampler_info{
			.sampler = *hiz.getSampler(),
		};

		auto params_info = m_cull_param_ubos[i]->getDescriptorInfo();
		auto indirect_info = m_indirect_buffers[i]->getDescriptorInfo();
		auto instance_info = m_instance_buffers[i]->getDescriptorInfo();
		auto count_info = m_draw_count_buffers[i]->getDescriptorInfo();

		VeDescriptorWriter(*m_compute_set_layout, pool)
			.writeBuffer(0, &obj_info)
			.writeBuffer(1, &transform_info)
			.writeBuffer(2, &params_info)
			.writeBuffer(3, &indirect_info)
			.writeBuffer(4, &instance_info)
			.writeBuffer(5, &count_info)
			.writeImage(6, &prev_hiz_info)
			.writeImage(7, &sampler_info)
			.writeBuffer(8, &active_id_info)
			.writeBuffer(9, &draw_group_info)
			.build(m_hiz_descriptor_sets[i]);
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

void GpuCullingSystem::refreshMainIndirectBuffer(vk::raii::CommandBuffer& cmd,
                                                  GpuSceneManager& scene_mgr, uint32_t frame) {
	uint32_t total_groups = scene_mgr.getTotalGroups();
	if (total_groups == 0)
		return;
	vk::DeviceSize copy_size = static_cast<vk::DeviceSize>(total_groups) * sizeof(VkDrawIndexedIndirectCommand);
	vk::BufferCopy copy{0, 0, copy_size};
	cmd.copyBuffer(scene_mgr.getIndirectTemplateBuffer(frame).getBuffer(),
	               m_indirect_buffers[frame]->getBuffer(), copy);
}

void GpuCullingSystem::dispatch(vk::raii::CommandBuffer& cmd, VeFrameInfo& frame_info, GpuSceneManager& scene_mgr) {
	uint32_t object_count = scene_mgr.getObjectCount();
	if (object_count == 0)
		return;

	uint32_t frame = frame_info.current_frame;

	// Update cull params UBO
	const CameraView& cv = frame_info.camera_view;
	glm::mat4 vp = cv.proj * cv.view;
	FrustumPlane cpu_planes[6];
	extractFrustumPlanes(vp, cpu_planes);

	const glm::mat4& proj = cv.proj;

	CullParams params{};
	for (int i = 0; i < 6; i++)
		params.frustum_planes[i] = cpu_planes[i].plane;
	params.view_proj = vp;
	params.view = cv.view;
	params.prev_view = m_frame_views[(frame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT];
	params.p00 = proj[0][0];
	params.p11 = proj[1][1];
	params.p22 = proj[2][2];
	params.p32 = proj[3][2];
	params.object_count = object_count;
	params.hiz_enabled = m_hiz_enabled ? 1 : 0;
	params.lod_bias = 0;
	if (m_hiz_enabled) {
		params.hiz_size = m_hiz_size;
		params.hiz_uv_scale = m_hiz_uv_scale;
		params.hiz_mip_count = m_hiz_mip_count;
	}
	m_cull_param_ubos[frame]->writeToBuffer(&params);
	m_frame_views[frame] = params.view;

	refreshMainIndirectBuffer(cmd, scene_mgr, frame);

	cmd.fillBuffer(m_draw_count_buffers[frame]->getBuffer(), 0,
		static_cast<vk::DeviceSize>(BUCKET_COUNT + 1) * sizeof(uint32_t), 0);

	if (m_compaction_enabled)
		cmd.fillBuffer(m_compact_count_buffers[frame]->getBuffer(), 0,
			static_cast<vk::DeviceSize>(BUCKET_COUNT) * sizeof(uint32_t), 0);

	vk::MemoryBarrier2 transfer_to_compute{
		.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
		.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
	};
	vk::DependencyInfo transfer_to_compute_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &transfer_to_compute};
	cmd.pipelineBarrier2(transfer_to_compute_dep);

	// Bind and dispatch (same pipeline for frustum-only and Hi-Z paths)
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	if (m_hiz_enabled)
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout,
			0, {*m_hiz_descriptor_sets[frame]}, {});
	else
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout,
			0, {*m_compute_descriptor_sets[frame]}, {});

	uint32_t group_count = (object_count + GPU_CULL_WORKGROUP_SIZE - 1) / GPU_CULL_WORKGROUP_SIZE;
	cmd.dispatch(group_count, 1, 1);

	// Compaction pass: pack non-empty draw commands contiguously
	if (m_compaction_enabled)
		dispatchCompaction(cmd, scene_mgr, frame);

	// Barrier: compute writes -> draw indirect reads + vertex shader reads + transfer (stats readback)
	vk::MemoryBarrier2 draw_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect
			| vk::PipelineStageFlagBits2::eVertexShader
			| vk::PipelineStageFlagBits2::eTransfer,
		.dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead
			| vk::AccessFlagBits2::eShaderStorageRead
			| vk::AccessFlagBits2::eTransferRead,
	};
	vk::DependencyInfo draw_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &draw_barrier};
	cmd.pipelineBarrier2(draw_dep);

	// Copy draw counts + triangle count to host-visible staging for async stats readback
	vk::BufferCopy count_copy{0, 0, static_cast<vk::DeviceSize>(BUCKET_COUNT + 1) * sizeof(uint32_t)};
	cmd.copyBuffer(m_draw_count_buffers[frame]->getBuffer(),
		m_readback_buffers[frame]->getBuffer(), count_copy);
}

void GpuCullingSystem::dispatchCompaction(vk::raii::CommandBuffer& cmd, GpuSceneManager& scene_mgr,
                                           uint32_t frame) {
	uint32_t total_groups = scene_mgr.getTotalGroups();
	if (total_groups == 0)
		return;

	// Barrier: cull compute writes -> compaction compute reads
	vk::MemoryBarrier2 cull_to_compact{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
	};
	vk::DependencyInfo barrier_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &cull_to_compact};
	cmd.pipelineBarrier2(barrier_dep);

	CompactPushConstants pc{.total_groups = total_groups, .bucket_offsets = {}, .bucket_counts = {}};
	for (uint32_t b = 0; b < BUCKET_COUNT; b++) {
		pc.bucket_offsets[b] = scene_mgr.getBucketGroupOffset(b);
		pc.bucket_counts[b] = scene_mgr.getBucketGroupCount(b);
	}

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compact_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_compact_pipeline_layout,
		0, {*m_compact_descriptor_sets[frame]}, {});
	cmd.pushConstants(
		*m_compact_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
		0, vk::ArrayProxy<const uint8_t>(sizeof(pc), reinterpret_cast<const uint8_t*>(&pc)));

	uint32_t wg_count = (total_groups + GPU_CULL_WORKGROUP_SIZE - 1) / GPU_CULL_WORKGROUP_SIZE;
	cmd.dispatch(wg_count, 1, 1);
}

void GpuCullingSystem::dispatchShadowCompaction(vk::raii::CommandBuffer& cmd, GpuSceneManager& scene_mgr,
                                                 uint32_t frame, uint32_t slot) {
	uint32_t total_groups = scene_mgr.getTotalGroups();
	if (total_groups == 0)
		return;

	// Clear shadow compact count buffer
	cmd.fillBuffer(m_shadow_compact_count_buffers[frame][slot]->getBuffer(), 0,
		static_cast<vk::DeviceSize>(BUCKET_COUNT) * sizeof(uint32_t), 0);

	// Barrier: fill -> compute
	vk::MemoryBarrier2 clear_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
		.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
	};
	vk::DependencyInfo clear_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &clear_barrier};
	cmd.pipelineBarrier2(clear_dep);

	CompactPushConstants pc{.total_groups = total_groups, .bucket_offsets = {}, .bucket_counts = {}};
	for (uint32_t b = 0; b < BUCKET_COUNT; b++) {
		pc.bucket_offsets[b] = scene_mgr.getBucketGroupOffset(b);
		pc.bucket_counts[b] = scene_mgr.getBucketGroupCount(b);
	}

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compact_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_compact_pipeline_layout,
		0, {*m_shadow_compact_descriptor_sets[frame][slot]}, {});
	cmd.pushConstants(
		*m_compact_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
		0, vk::ArrayProxy<const uint8_t>(sizeof(pc), reinterpret_cast<const uint8_t*>(&pc)));

	uint32_t wg_count = (total_groups + GPU_CULL_WORKGROUP_SIZE - 1) / GPU_CULL_WORKGROUP_SIZE;
	cmd.dispatch(wg_count, 1, 1);
}

void GpuCullingSystem::dispatchShadowCulls(vk::raii::CommandBuffer& cmd,
                                            const ShadowCullRequest* requests, uint32_t count,
                                            GpuSceneManager& scene_mgr,
                                            uint32_t frame_index) {
	uint32_t object_count = scene_mgr.getObjectCount();
	if (object_count == 0 || count == 0)
		return;

	vk::MemoryBarrier2 entry_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eDrawIndirect
			| vk::PipelineStageFlagBits2::eVertexShader
			| vk::PipelineStageFlagBits2::eComputeShader
			| vk::PipelineStageFlagBits2::eTransfer,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite
			| vk::AccessFlagBits2::eTransferWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader
			| vk::PipelineStageFlagBits2::eTransfer,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead
			| vk::AccessFlagBits2::eShaderStorageWrite
			| vk::AccessFlagBits2::eTransferWrite,
	};
	vk::DependencyInfo entry_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &entry_barrier};
	cmd.pipelineBarrier2(entry_dep);

	uint32_t total_groups = scene_mgr.getTotalGroups();
	vk::DeviceSize copy_size = static_cast<vk::DeviceSize>(total_groups)
	                         * sizeof(VkDrawIndexedIndirectCommand);

	// Loop 1: write UBOs and copy indirect template for every request.
	for (uint32_t r = 0; r < count; r++) {
		const auto& req = requests[r];
		uint32_t slot = req.slot;

		FrustumPlane cpu_planes[6];
		extractFrustumPlanes(req.view_proj, cpu_planes);

		CullParams params{};
		for (int i = 0; i < 6; i++)
			params.frustum_planes[i] = cpu_planes[i].plane;
		params.view_proj = req.view_proj;
		params.object_count = object_count;
		params.is_shadow_pass = static_cast<uint32_t>(req.shadow_mode);
		params.lod_bias = req.lod_bias;

		if (req.camera_view != nullptr && m_hiz_enabled) {
			params.hiz_enabled = 1;
			params.view = req.camera_view->view;
			params.prev_view = m_frame_views[
				(frame_index + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT];
			const glm::mat4& proj = req.camera_view->proj;
			params.p00 = proj[0][0];
			params.p11 = proj[1][1];
			params.p22 = proj[2][2];
			params.p32 = proj[3][2];
			params.hiz_size = m_hiz_size;
			params.hiz_uv_scale = m_hiz_uv_scale;
			params.hiz_mip_count = m_hiz_mip_count;
		} else {
			params.hiz_enabled = 0;
		}

		cmd.updateBuffer<CullParams>(
			m_shadow_cull_param_ubos[frame_index][slot]->getBuffer(),
			vk::DeviceSize{0}, params);

		if (total_groups > 0) {
			cmd.copyBuffer(scene_mgr.getIndirectTemplateBuffer(frame_index).getBuffer(),
			               m_shadow_indirect_buffers[frame_index][slot]->getBuffer(),
			               vk::BufferCopy{0, 0, copy_size});
		}

		m_shadow_dispatched_slots_mask |= (1u << slot);
	}

	// Single transfer -> compute barrier covering all UBO writes + copies.
	vk::MemoryBarrier2 ubo_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
		.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eUniformRead
			| vk::AccessFlagBits2::eShaderStorageRead
			| vk::AccessFlagBits2::eShaderStorageWrite,
	};
	vk::DependencyInfo ubo_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &ubo_barrier};
	cmd.pipelineBarrier2(ubo_dep);

	// Loop 2: bind pipeline once, dispatch per slot, no inter-dispatch barriers
	// so successive cull dispatches can overlap on the GPU.
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	uint32_t group_count = (object_count + GPU_CULL_WORKGROUP_SIZE - 1) / GPU_CULL_WORKGROUP_SIZE;
	for (uint32_t r = 0; r < count; r++) {
		uint32_t slot = requests[r].slot;
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout,
			0, {*m_shadow_compute_descriptor_sets[frame_index][slot]}, {});
		cmd.dispatch(group_count, 1, 1);
	}
	// Caller must call flushShadowCullBarrier() before reading the output buffers.
}

void GpuCullingSystem::flushShadowCullBarrier(vk::raii::CommandBuffer& cmd, GpuSceneManager& scene_mgr,
                                               uint32_t frame_index) {
	uint32_t mask = m_shadow_dispatched_slots_mask;
	m_shadow_dispatched_slots_mask = 0;

	if (m_compaction_enabled && mask != 0) {
		// Barrier: all shadow cull compute writes -> compaction compute reads
		vk::MemoryBarrier2 cull_to_compact{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader
				| vk::PipelineStageFlagBits2::eTransfer,
			.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead
				| vk::AccessFlagBits2::eShaderStorageWrite
				| vk::AccessFlagBits2::eTransferWrite,
		};
		vk::DependencyInfo barrier_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &cull_to_compact};
		cmd.pipelineBarrier2(barrier_dep);

		for (uint32_t slot = 0; slot < SHADOW_BUFFER_COUNT; slot++) {
			if ((mask & (1u << slot)) == 0)
				continue;
			dispatchShadowCompaction(cmd, scene_mgr, frame_index, slot);
		}
	}

	// Final compute->draw 
	vk::MemoryBarrier2 draw_barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader,
		.dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead,
	};
	vk::DependencyInfo draw_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &draw_barrier};
	cmd.pipelineBarrier2(draw_dep);
}

uint32_t GpuCullingSystem::readbackDrawCounts(uint32_t frame) const {
	const auto* counts = static_cast<const uint32_t*>(m_readback_buffers[frame]->getMappedMemory());
	uint32_t total = 0;
	for (uint32_t b = 0; b < BUCKET_COUNT; b++)
		total += counts[b];
	return total;
}

uint32_t GpuCullingSystem::readbackTriangleCount(uint32_t frame) const {
	const auto* counts = static_cast<const uint32_t*>(m_readback_buffers[frame]->getMappedMemory());
	return counts[BUCKET_COUNT] / 3;
}

const uint32_t* GpuCullingSystem::getReadbackCounts(uint32_t frame) const {
	return static_cast<const uint32_t*>(m_readback_buffers[frame]->getMappedMemory());
}

void GpuCullingSystem::clearReadback() {
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		std::memset(m_readback_buffers[i]->getMappedMemory(), 0,
			(BUCKET_COUNT + 1) * sizeof(uint32_t));
}

void GpuCullingSystem::subscribeToEvents(EventBus& event_bus, HizSystem& hiz, GpuSceneManager& scene_mgr) {
	m_event_bus = &event_bus;
	m_resolution_sub = event_bus.subscribe<ResolutionChangedEvent>(
		[this, &hiz, &scene_mgr](const ResolutionChangedEvent& e) {
			createHizDescriptorSets(e.pool, scene_mgr, hiz);
			createShadowHizDescriptorSets(e.pool, scene_mgr, hiz);
		});
	m_scene_unloaded_sub = event_bus.subscribe<SceneUnloadedEvent>(
		[this](const SceneUnloadedEvent&) {
			clearReadback();
		});
}

} // namespace ve
