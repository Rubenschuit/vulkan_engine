#include "pch.hpp"
#include "rendering/meshlet_culling_system.hpp"
#include "rendering/gpu_culling_system.hpp"
#include "rendering/gpu_scene_manager.hpp"
#include "rendering/pbr_mega_buffer.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/ve_frame_info.hpp"
#include "vulkan/ve_image.hpp"
#include "utils/ve_frustum.hpp"
#include "utils/ve_log.hpp"

namespace ve {

MeshletCullingSystem::MeshletCullingSystem(VeDevice& device, const std::filesystem::path& shaders_dir)
	: m_ve_device(device) {

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_visible_objects[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(VisibleObjectEntry), MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_meshlet_object_map[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(uint32_t), MAX_MESHLET_DRAWS,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_counts[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(uint32_t), 2,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst
				| vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_dispatch_indirect[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(uint32_t), 3,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
				| vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_meshlet_indirect[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(VkDrawIndexedIndirectCommand), MAX_MESHLET_DRAWS,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
				| vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_meshlet_draw_counts[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(uint32_t), BUCKET_COUNT + 1,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
				| vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_instance_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(InstanceData), MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_cull_param_ubos[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(CullParams), 1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment);
		m_cull_param_ubos[i]->map();

		// Staging buffer for async readback stats
		m_readback_staging[i] = std::make_unique<VeBuffer>(m_ve_device,
			sizeof(uint32_t), BUCKET_COUNT + 2,
			vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_readback_staging[i]->map();

		// Shadow culling buffers: one set per shadow layer
		for (uint32_t slot = 0; slot < SHADOW_BUFFER_COUNT; slot++) {
			m_shadow_visible_objects[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(VisibleObjectEntry), MAX_GPU_OBJECTS,
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_shadow_meshlet_object_map[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(uint32_t), MAX_MESHLET_SHADOW_DRAWS,
				vk::BufferUsageFlagBits::eStorageBuffer,
				vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_shadow_counts[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(uint32_t), 2,
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_shadow_dispatch_indirect[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(uint32_t), 3,
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
					| vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_shadow_meshlet_indirect[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(VkDrawIndexedIndirectCommand), MAX_MESHLET_SHADOW_DRAWS,
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
					| vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_shadow_meshlet_draw_counts[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(uint32_t), BUCKET_COUNT,
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer
					| vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
				vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_shadow_instance_buffers[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(InstanceData), MAX_GPU_OBJECTS,
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal);

			m_shadow_cull_param_ubos[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(CullParams), 1,
				vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eDeviceLocal,
				m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment);

			m_shadow_readback_staging[i][slot] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(uint32_t), MESHLET_SHADOW_BUCKET_COUNT,
				vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
			m_shadow_readback_staging[i][slot]->map();
		}
	}

	// Initialize high-water marks with a small cap for the drawIndexedIndirect fallback.
	if (!m_ve_device.supportsDrawIndirectCount()) {
		constexpr uint32_t INITIAL_HW = 8192;
		m_readback_high_water.fill(INITIAL_HW);
		for (auto& hw : m_shadow_readback_high_water)
			hw.fill(INITIAL_HW);
	}

	// Pass 1 layout: objects, transforms, cull_params, active_ids, meshlet_obj_info,
	//                visible_objects(RW), counts(RW), instance_data(RW), hiz_image, hiz_sampler,
	//                meshlet_object_map(RW), dispatch_indirect(RW)
	m_pass1_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(2,  vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(3,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(4,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(5,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(6,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(7,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(8,  vk::DescriptorType::eSampledImage,  vk::ShaderStageFlagBits::eCompute)
		.addBinding(9,  vk::DescriptorType::eSampler,       vk::ShaderStageFlagBits::eCompute)
		.addBinding(10, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(11, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.build();

	// Pass 2 layout: visible_objects, meshlet_object_map, counts, meshlet_ssbo, objects, transforms,
	//                cull_params, hiz_image, hiz_sampler, indirect_cmds(RW), draw_counts(RW)
	m_pass2_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(2,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(3,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(4,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(5,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(6,  vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(7,  vk::DescriptorType::eSampledImage,  vk::ShaderStageFlagBits::eCompute)
		.addBinding(8,  vk::DescriptorType::eSampler,       vk::ShaderStageFlagBits::eCompute)
		.addBinding(9,  vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(10, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.build();

	createPipelineLayouts();

	m_pass1_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, shaders_dir / "gpu_cull_meshlet_comp.spv", m_pass1_pipeline_layout);
	m_pass2_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, shaders_dir / "meshlet_cull_comp.spv", m_pass2_pipeline_layout);

	// Dummy 1x1 R32Float image + sampler for Hi-Z placeholder bindings
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

MeshletCullingSystem::~MeshletCullingSystem() = default;

void MeshletCullingSystem::createPipelineLayouts() {
	vk::DescriptorSetLayout pass1_raw = *m_pass1_layout->getDescriptorSetLayout();
	vk::PipelineLayoutCreateInfo pass1_info{
		.setLayoutCount = 1,
		.pSetLayouts    = &pass1_raw,
	};
	m_pass1_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pass1_info);

	vk::DescriptorSetLayout pass2_raw = *m_pass2_layout->getDescriptorSetLayout();
	vk::PipelineLayoutCreateInfo pass2_info{
		.setLayoutCount = 1,
		.pSetLayouts    = &pass2_raw,
	};
	m_pass2_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pass2_info);
}

void MeshletCullingSystem::writeCullDescriptorSets(
		VeDescriptorPool& pool, GpuSceneManager& scene_mgr, const PbrMegaBuffer& mega_buffer,
		uint32_t frame,
		vk::DescriptorImageInfo& hiz_img, vk::DescriptorImageInfo& hiz_smp_info,
		VeBuffer& cull_params, VeBuffer& visible_objects, VeBuffer& counts,
		VeBuffer& instance_buf, VeBuffer& meshlet_object_map, VeBuffer& dispatch_indirect,
		VeBuffer& meshlet_indirect, VeBuffer& meshlet_draw_counts,
		vk::raii::DescriptorSet& out_pass1, vk::raii::DescriptorSet& out_pass2) {
	auto obj_info    = scene_mgr.getObjectDataBuffer(frame).getDescriptorInfo();
	auto xfm_info    = scene_mgr.getTransformBuffer(frame).getDescriptorInfo();
	auto params_info = cull_params.getDescriptorInfo();
	auto ids_info    = scene_mgr.getActiveIdBuffer(frame).getDescriptorInfo();
	auto moi_info    = scene_mgr.getMeshletObjectInfoBuffer(frame).getDescriptorInfo();
	auto vis_info    = visible_objects.getDescriptorInfo();
	auto cnt_info    = counts.getDescriptorInfo();
	auto inst_info   = instance_buf.getDescriptorInfo();
	auto map_info    = meshlet_object_map.getDescriptorInfo();
	auto disp_info   = dispatch_indirect.getDescriptorInfo();

	VeDescriptorWriter(*m_pass1_layout, pool)
		.writeBuffer(0,  &obj_info)
		.writeBuffer(1,  &xfm_info)
		.writeBuffer(2,  &params_info)
		.writeBuffer(3,  &ids_info)
		.writeBuffer(4,  &moi_info)
		.writeBuffer(5,  &vis_info)
		.writeBuffer(6,  &cnt_info)
		.writeBuffer(7,  &inst_info)
		.writeImage(8,   &hiz_img)
		.writeImage(9,   &hiz_smp_info)
		.writeBuffer(10, &map_info)
		.writeBuffer(11, &disp_info)
		.build(out_pass1);

	if (mega_buffer.hasMeshletData()) {
		auto msbo_info = mega_buffer.getMeshletSsbo()->getDescriptorInfo();
		auto ind_info  = meshlet_indirect.getDescriptorInfo();
		auto dc_info   = meshlet_draw_counts.getDescriptorInfo();

		VeDescriptorWriter(*m_pass2_layout, pool)
			.writeBuffer(0,  &vis_info)
			.writeBuffer(1,  &map_info)
			.writeBuffer(2,  &cnt_info)
			.writeBuffer(3,  &msbo_info)
			.writeBuffer(4,  &obj_info)
			.writeBuffer(5,  &xfm_info)
			.writeBuffer(6,  &params_info)
			.writeImage(7,   &hiz_img)
			.writeImage(8,   &hiz_smp_info)
			.writeBuffer(9,  &ind_info)
			.writeBuffer(10, &dc_info)
			.build(out_pass2);
	}
}

void MeshletCullingSystem::createDescriptorSets(VeDescriptorPool& pool,
                                                GpuSceneManager& scene_mgr,
                                                const PbrMegaBuffer& mega_buffer) {
	vk::DescriptorImageInfo dummy_img{.imageView = *m_dummy_image->getImageView(), .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::DescriptorImageInfo dummy_smp_info{.sampler = *m_dummy_sampler};

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		writeCullDescriptorSets(pool, scene_mgr, mega_buffer, i,
			dummy_img, dummy_smp_info,
			*m_cull_param_ubos[i], *m_visible_objects[i], *m_counts[i],
			*m_instance_buffers[i], *m_meshlet_object_map[i], *m_dispatch_indirect[i],
			*m_meshlet_indirect[i], *m_meshlet_draw_counts[i],
			m_pass1_sets[i], m_pass2_sets[i]);
}

void MeshletCullingSystem::createHizDescriptorSets(VeDescriptorPool& pool,
                                                   GpuSceneManager& scene_mgr,
                                                   const PbrMegaBuffer& mega_buffer,
                                                   HizSystem& hiz) {
	m_hiz_size      = glm::vec2(static_cast<float>(hiz.getWidth()), static_cast<float>(hiz.getHeight()));
	m_hiz_mip_count = hiz.getMipLevels();

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		uint32_t prev_frame = (i + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
		vk::DescriptorImageInfo hiz_img{.imageView = *hiz.getHizImageView(prev_frame), .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
		vk::DescriptorImageInfo hiz_smp_info{.sampler = *hiz.getSampler()};

		writeCullDescriptorSets(pool, scene_mgr, mega_buffer, i,
			hiz_img, hiz_smp_info,
			*m_cull_param_ubos[i], *m_visible_objects[i], *m_counts[i],
			*m_instance_buffers[i], *m_meshlet_object_map[i], *m_dispatch_indirect[i],
			*m_meshlet_indirect[i], *m_meshlet_draw_counts[i],
			m_pass1_hiz_sets[i], m_pass2_hiz_sets[i]);
	}
}

void MeshletCullingSystem::createGlobalDescriptorSets(VeDescriptorPool& pool,
                                                      VeDescriptorSetLayout& global_layout,
                                                      std::vector<std::unique_ptr<VeBuffer>>& ubo_buffers,
                                                      VeBuffer& material_ssbo) {
	auto material_info = material_ssbo.getDescriptorInfo();
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto ubo_info      = ubo_buffers[i]->getDescriptorInfo();
		auto instance_info = m_instance_buffers[i]->getDescriptorInfo();

		VeDescriptorWriter(global_layout, pool)
			.writeBuffer(0, &ubo_info)
			.writeBuffer(1, &instance_info)
			.writeBuffer(2, &material_info)
			.build(m_global_descriptor_sets[i]);
	}
}

void MeshletCullingSystem::dispatch(vk::raii::CommandBuffer& cmd, VeFrameInfo& frame_info,
                                    GpuSceneManager& scene_mgr) {
	uint32_t object_count = scene_mgr.getObjectCount();
	if (object_count == 0)
		return;

	uint32_t frame = frame_info.current_frame;

	// Update cull params UBO
	glm::mat4 vp = frame_info.camera.getProj() * frame_info.camera.getView();
	FrustumPlane cpu_planes[6];
	extractFrustumPlanes(vp, cpu_planes);

	const glm::mat4& proj = frame_info.camera.getProj();

	CullParams params{};
	for (int i = 0; i < 6; i++)
		params.frustum_planes[i] = cpu_planes[i].plane;
	params.view_proj      = vp;
	params.view           = frame_info.camera.getView();
	params.prev_view      = m_frame_views[(frame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT];
	params.p00            = proj[0][0];
	params.p11            = proj[1][1];
	params.p22            = proj[2][2];
	params.p32            = proj[3][2];
	params.object_count   = object_count;
	params.hiz_enabled    = m_hiz_enabled ? 1 : 0;
	params.is_shadow_pass = 0;
	params.lod_bias       = 0;
	params.max_meshlet_draws = MAX_MESHLET_DRAWS;
	params.bucket_count      = MESHLET_BUCKET_COUNT;
	params.camera_pos     = glm::vec4(frame_info.camera.getPosition(), 0.0f);
	if (m_hiz_enabled) {
		params.hiz_size      = m_hiz_size;
		params.hiz_mip_count = m_hiz_mip_count;
	}
	m_cull_param_ubos[frame]->writeToBuffer(&params);
	m_frame_views[frame] = params.view;

	// Async readback: read draw counts written 2 frames ago from the mapped staging buffer
	m_current_readback_valid = false;
	auto* staging_ptr = static_cast<const uint32_t*>(m_readback_staging[frame]->getMappedMemory());
	if (staging_ptr && m_has_readback[frame]) {
		std::memcpy(m_readback_counts.data(), staging_ptr, BUCKET_COUNT * sizeof(uint32_t));
		m_readback_tri_count = staging_ptr[BUCKET_COUNT];
		m_readback_visible_objects = staging_ptr[BUCKET_COUNT + 1];
		m_current_readback_valid = true;

		// Update high-water marks: jump directly to 2x actual + headroom
		constexpr uint32_t MAX_PER_BUCKET = MAX_MESHLET_DRAWS / BUCKET_COUNT;
		for (uint32_t b = 0; b < BUCKET_COUNT; b++)
			m_readback_high_water[b] = std::min(
				m_readback_counts[b] * 2 + 1024,
				MAX_PER_BUCKET);
	}

	// Clear per-frame counters and init dispatch_indirect to (0, 1, 1)
	cmd.fillBuffer(m_counts[frame]->getBuffer(), 0, 2 * sizeof(uint32_t), 0);
	cmd.fillBuffer(m_meshlet_draw_counts[frame]->getBuffer(), 0,
		static_cast<vk::DeviceSize>(BUCKET_COUNT + 1) * sizeof(uint32_t), 0);
	cmd.fillBuffer(m_dispatch_indirect[frame]->getBuffer(), 0, 4, 0u);  // groups_x = 0
	cmd.fillBuffer(m_dispatch_indirect[frame]->getBuffer(), 4, 8, 1u);  // Y = 1, Z = 1

	// Without drawIndirectCount, zero-fill the indirect buffer region using high-water marks
	if (!m_ve_device.supportsDrawIndirectCount()) {
		constexpr uint32_t MAX_PER_BUCKET = MAX_MESHLET_DRAWS / BUCKET_COUNT;
		constexpr vk::DeviceSize CMD_SIZE = sizeof(VkDrawIndexedIndirectCommand);
		for (uint32_t b = 0; b < BUCKET_COUNT; b++) {
			uint32_t count = m_current_readback_valid ? m_readback_high_water[b] : MAX_PER_BUCKET;
			auto offset = static_cast<vk::DeviceSize>(b) * MAX_PER_BUCKET * CMD_SIZE;
			cmd.fillBuffer(m_meshlet_indirect[frame]->getBuffer(), offset,
				static_cast<vk::DeviceSize>(count) * CMD_SIZE, 0);
		}
	}

	// Transfer -> Compute barrier
	vk::MemoryBarrier2 clear_barrier{
		.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer,
		.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
		.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
	};
	vk::DependencyInfo clear_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &clear_barrier};
	cmd.pipelineBarrier2(clear_dep);

	// Pass 1: per-object frustum + Hi-Z cull, writes VisibleObjectEntry + InstanceData +
	//         meshlet_object_map + dispatch_indirect (via InterlockedMax)
	auto& pass1_set = (m_hiz_enabled && *m_pass1_hiz_sets[frame]) ? m_pass1_hiz_sets[frame] : m_pass1_sets[frame];
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pass1_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pass1_pipeline_layout,
		0, {*pass1_set}, {});
	uint32_t groups1 = (object_count + MESHLET_CULL_WORKGROUP_SIZE - 1) / MESHLET_CULL_WORKGROUP_SIZE;
	cmd.dispatch(groups1, 1, 1);

	// Compute -> Compute + DrawIndirect barrier (pass 2 reads dispatch_indirect via dispatchIndirect)
	vk::MemoryBarrier2 p1_barrier{
		.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
		.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
		                | vk::AccessFlagBits2::eIndirectCommandRead,
	};
	vk::DependencyInfo p1_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &p1_barrier};
	cmd.pipelineBarrier2(p1_dep);

	// Pass 2: per-meshlet frustum + cone + Hi-Z cull, writes draw commands
	auto& pass2_set = (m_hiz_enabled && *m_pass2_hiz_sets[frame]) ? m_pass2_hiz_sets[frame] : m_pass2_sets[frame];
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pass2_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pass2_pipeline_layout,
		0, {*pass2_set}, {});
	cmd.dispatchIndirect(m_dispatch_indirect[frame]->getBuffer(), 0);

	// Compute -> DrawIndirect + VertexShader + Transfer barrier (for cpu readback)
	vk::MemoryBarrier2 draw_barrier{
		.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
		.dstStageMask  = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader
		               | vk::PipelineStageFlagBits2::eTransfer,
		.dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead
		               | vk::AccessFlagBits2::eTransferRead,
	};
	vk::DependencyInfo draw_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &draw_barrier};
	cmd.pipelineBarrier2(draw_dep);

	// Copy draw counts + triangle count to staging for CPU readback next time this frame index is used
	vk::BufferCopy region{0, 0, static_cast<vk::DeviceSize>(BUCKET_COUNT + 1) * sizeof(uint32_t)};
	cmd.copyBuffer(m_meshlet_draw_counts[frame]->getBuffer(),
		m_readback_staging[frame]->getBuffer(), region);

	// Copy visible object count (from pass 1) to staging for stats readback
	vk::BufferCopy obj_region{0, static_cast<vk::DeviceSize>(BUCKET_COUNT + 1) * sizeof(uint32_t), sizeof(uint32_t)};
	cmd.copyBuffer(m_counts[frame]->getBuffer(), m_readback_staging[frame]->getBuffer(), obj_region);
	m_has_readback[frame] = true;
}

void MeshletCullingSystem::createShadowDescriptorSets(VeDescriptorPool& pool,
                                                       GpuSceneManager& scene_mgr,
                                                       const PbrMegaBuffer& mega_buffer) {
	vk::DescriptorImageInfo dummy_img{.imageView = *m_dummy_image->getImageView(), .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::DescriptorImageInfo dummy_smp_info{.sampler = *m_dummy_sampler};

	m_shadow_pass1_sets.resize(MAX_FRAMES_IN_FLIGHT);
	m_shadow_pass2_sets.resize(MAX_FRAMES_IN_FLIGHT);

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_shadow_pass1_sets[i].clear();
		m_shadow_pass2_sets[i].clear();

		for (uint32_t slot = 0; slot < SHADOW_BUFFER_COUNT; slot++) {
			vk::raii::DescriptorSet ds1{nullptr};
			vk::raii::DescriptorSet ds2{nullptr};
			writeCullDescriptorSets(pool, scene_mgr, mega_buffer, i,
				dummy_img, dummy_smp_info,
				*m_shadow_cull_param_ubos[i][slot], *m_shadow_visible_objects[i][slot],
				*m_shadow_counts[i][slot], *m_shadow_instance_buffers[i][slot],
				*m_shadow_meshlet_object_map[i][slot], *m_shadow_dispatch_indirect[i][slot],
				*m_shadow_meshlet_indirect[i][slot], *m_shadow_meshlet_draw_counts[i][slot],
				ds1, ds2);
			m_shadow_pass1_sets[i].push_back(std::move(ds1));
			if (mega_buffer.hasMeshletData())
				m_shadow_pass2_sets[i].push_back(std::move(ds2));
		}
	}
}

void MeshletCullingSystem::createShadowGlobalDescriptorSets(
		VeDescriptorPool& pool,
		VeDescriptorSetLayout& layout,
		std::vector<std::vector<std::unique_ptr<VeBuffer>>>& csm_ubos,
		std::vector<std::vector<std::unique_ptr<VeBuffer>>>& shadow_ubos) {
	m_shadow_global_sets.resize(MAX_FRAMES_IN_FLIGHT);
	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		m_shadow_global_sets[frame].clear();
		m_shadow_global_sets[frame].reserve(SHADOW_BUFFER_COUNT);

		// Cascade slots: 0..NUM_CSM_CASCADES-1
		for (uint32_t cascade = 0; cascade < NUM_CSM_CASCADES; cascade++) {
			auto ubo_info  = csm_ubos[frame][cascade]->getDescriptorInfo();
			auto inst_info = m_shadow_instance_buffers[frame][cascade]->getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(layout, pool)
				.writeBuffer(0, &ubo_info)
				.writeBuffer(1, &inst_info)
				.build(ds);
			m_shadow_global_sets[frame].push_back(std::move(ds));
		}

		// Shadow light slots: NUM_CSM_CASCADES..SHADOW_BUFFER_COUNT-1
		for (uint32_t light = 0; light < MAX_SHADOW_LIGHTS; light++) {
			uint32_t slot  = NUM_CSM_CASCADES + light;
			auto ubo_info  = shadow_ubos[frame][slot]->getDescriptorInfo();
			auto inst_info = m_shadow_instance_buffers[frame][slot]->getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(layout, pool)
				.writeBuffer(0, &ubo_info)
				.writeBuffer(1, &inst_info)
				.build(ds);
			m_shadow_global_sets[frame].push_back(std::move(ds));
		}
	}
}

void MeshletCullingSystem::dispatchShadowCulls(vk::raii::CommandBuffer& cmd,
                                                const ShadowCullRequest* requests, uint32_t count,
                                                GpuSceneManager& scene_mgr,
                                                uint32_t frame_index,
                                                bool skip_readback) {
	uint32_t object_count = scene_mgr.getObjectCount();
	if (object_count == 0 || count == 0)
		return;

	uint32_t groups1 = (object_count + MESHLET_CULL_WORKGROUP_SIZE - 1) / MESHLET_CULL_WORKGROUP_SIZE;

	// Async readback: read shadow draw counts written 2 frames ago from mapped staging.
	if (!skip_readback) {
		for (uint32_t r = 0; r < count; r++) {
			uint32_t slot = requests[r].slot;
			if (m_shadow_readback_staging[frame_index][slot]) {
				auto* ptr = static_cast<const uint32_t*>(
					m_shadow_readback_staging[frame_index][slot]->getMappedMemory());
				if (ptr && m_shadow_has_readback[frame_index][slot]) {
					std::memcpy(m_shadow_readback_counts[slot].data(), ptr,
						MESHLET_SHADOW_BUCKET_COUNT * sizeof(uint32_t));

					// Update high-water marks: jump directly to 2x actual + headroom
					constexpr uint32_t SHADOW_MAX_PER_BUCKET = MAX_MESHLET_SHADOW_DRAWS / MESHLET_SHADOW_BUCKET_COUNT;
					for (uint32_t b = 0; b < MESHLET_SHADOW_BUCKET_COUNT; b++)
						m_shadow_readback_high_water[slot][b] = std::min(
							m_shadow_readback_counts[slot][b] * 2 + 1024,
							SHADOW_MAX_PER_BUCKET);
				}
			}
		}
	}

	// Write UBOs, clear counters, and init dispatch_indirect for all requests
	for (uint32_t r = 0; r < count; r++) {
		auto& req = requests[r];
		uint32_t slot = req.slot;

		FrustumPlane cpu_planes[6];
		extractFrustumPlanes(req.view_proj, cpu_planes);

		CullParams params{};
		for (int i = 0; i < 6; i++)
			params.frustum_planes[i] = cpu_planes[i].plane;
		params.view_proj         = req.view_proj;
		params.object_count      = object_count;
		params.is_shadow_pass    = static_cast<uint32_t>(req.shadow_mode);
		params.lod_bias          = req.lod_bias;
		params.max_meshlet_draws = MAX_MESHLET_SHADOW_DRAWS;
		params.bucket_count      = MESHLET_SHADOW_BUCKET_COUNT;
		params.camera_pos        = glm::vec4(req.light_pos, 0.0f);
		params.hiz_enabled       = 0;
		params.shadow_cone_cull  = (req.slot < NUM_CSM_CASCADES) ? 1 : 0;
		// Record UBO data into the command stream so each dispatch sees its own
		// parameters even when the same slot is reused for static and dynamic passes.
		cmd.updateBuffer<CullParams>(m_shadow_cull_param_ubos[frame_index][slot]->getBuffer(),
			vk::DeviceSize{0}, params);

		cmd.fillBuffer(m_shadow_counts[frame_index][slot]->getBuffer(),
			0, 2 * sizeof(uint32_t), 0);
		cmd.fillBuffer(m_shadow_meshlet_draw_counts[frame_index][slot]->getBuffer(),
			0, static_cast<vk::DeviceSize>(MESHLET_SHADOW_BUCKET_COUNT) * sizeof(uint32_t), 0);
		cmd.fillBuffer(m_shadow_dispatch_indirect[frame_index][slot]->getBuffer(), 0, 4, 0u);
		cmd.fillBuffer(m_shadow_dispatch_indirect[frame_index][slot]->getBuffer(), 4, 8, 1u);
		if (!m_ve_device.supportsDrawIndirectCount()) {
			constexpr uint32_t SHADOW_MAX_PER_BUCKET = MAX_MESHLET_SHADOW_DRAWS / MESHLET_SHADOW_BUCKET_COUNT;
			constexpr vk::DeviceSize CMD_SIZE = sizeof(VkDrawIndexedIndirectCommand);
			for (uint32_t b = 0; b < MESHLET_SHADOW_BUCKET_COUNT; b++) {
				uint32_t fill_count = m_shadow_has_readback[frame_index][slot]
					? m_shadow_readback_high_water[slot][b] : SHADOW_MAX_PER_BUCKET;
				auto offset = static_cast<vk::DeviceSize>(b) * SHADOW_MAX_PER_BUCKET * CMD_SIZE;
				cmd.fillBuffer(m_shadow_meshlet_indirect[frame_index][slot]->getBuffer(), offset,
					static_cast<vk::DeviceSize>(fill_count) * CMD_SIZE, 0);
			}
		}
	}

	// Transfer -> Compute
	vk::MemoryBarrier2 clear_barrier{
		.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer,
		.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
		.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
	};
	vk::DependencyInfo clear_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &clear_barrier};
	cmd.pipelineBarrier2(clear_dep);

	// Pass 1 dispatches
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pass1_pipeline->getPipeline());
	for (uint32_t r = 0; r < count; r++) {
		uint32_t slot = requests[r].slot;
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pass1_pipeline_layout,
			0, {*m_shadow_pass1_sets[frame_index][slot]}, {});
		cmd.dispatch(groups1, 1, 1);
	}

	// Compute -> Compute + DrawIndirect (pass 2 reads dispatch_indirect via dispatchIndirect)
	vk::MemoryBarrier2 p1_barrier{
		.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
		.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
		                | vk::AccessFlagBits2::eIndirectCommandRead,
	};
	vk::DependencyInfo p1_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &p1_barrier};
	cmd.pipelineBarrier2(p1_dep);

	// Pass 2 dispatches
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pass2_pipeline->getPipeline());
	for (uint32_t r = 0; r < count; r++) {
		uint32_t slot = requests[r].slot;
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pass2_pipeline_layout,
			0, {*m_shadow_pass2_sets[frame_index][slot]}, {});
		cmd.dispatchIndirect(m_shadow_dispatch_indirect[frame_index][slot]->getBuffer(), 0);
	}

	// Final barrier: Compute -> DrawIndirect + VertexShader + Transfer (for staging copy)
	vk::MemoryBarrier2 draw_barrier{
		.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
		.dstStageMask  = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader
		               | vk::PipelineStageFlagBits2::eTransfer,
		.dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead
		               | vk::AccessFlagBits2::eTransferRead,
	};
	vk::DependencyInfo draw_dep{.memoryBarrierCount = 1, .pMemoryBarriers = &draw_barrier};
	cmd.pipelineBarrier2(draw_dep);

	if (!skip_readback) {
		for (uint32_t r = 0; r < count; r++) {
			uint32_t slot = requests[r].slot;
			vk::BufferCopy region{0, 0,
				static_cast<vk::DeviceSize>(MESHLET_SHADOW_BUCKET_COUNT) * sizeof(uint32_t)};
			cmd.copyBuffer(m_shadow_meshlet_draw_counts[frame_index][slot]->getBuffer(),
				m_shadow_readback_staging[frame_index][slot]->getBuffer(), region);
			m_shadow_has_readback[frame_index][slot] = true;
		}
	}
}

} // namespace ve