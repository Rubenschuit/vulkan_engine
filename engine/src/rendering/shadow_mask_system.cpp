#include "pch.hpp"
#include "rendering/shadow_mask_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "platform/ve_file_system.hpp"
#include "utils/ve_log.hpp"

#include <cmath>

namespace ve {

struct ShadowMaskPushConstant {
	glm::vec2 mask_size;   // shadow mask output resolution (may be half-res)
	glm::vec2 depth_size;  // depth buffer resolution (always full screen)
};

ShadowMaskSystem::ShadowMaskSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	VeResourceManager& resource_manager,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout,
	std::filesystem::path shader_path,
	vk::Extent2D mask_extent,
	vk::Extent2D depth_extent,
	vk::SampleCountFlagBits depth_sample_count,
	const vk::raii::ImageView& depth_image_view,
	const vk::raii::Image& depth_image)
	: m_ve_device(device), m_shader_path(std::move(shader_path)),
	  m_extent(mask_extent), m_depth_extent(depth_extent), m_depth_sample_count(depth_sample_count) {

	m_depth_image = *depth_image;
	m_depth_image_view = *depth_image_view;
	m_has_ms_support = m_ve_device.supportsStorageImageMultisample();
	m_default_mask_texture = resource_manager.load<VeTexture>("default_albedo");
	createShadowMaskImage(m_extent);
	createComputeSetLayout();
	createOutputSetLayout();
	createSampler();
	createPipelineLayout(global_set_layout, shadow_set_layout);
	createPipelines();
	createComputeUBOs(descriptor_pool, global_set_layout);
	createDescriptorSets(descriptor_pool, global_set_layout);
}

ShadowMaskSystem::~ShadowMaskSystem() = default;

void ShadowMaskSystem::createShadowMaskImage(vk::Extent2D extent) {
	m_shadow_mask_image = std::make_unique<VeImage>(
		m_ve_device,
		extent.width,
		extent.height,
		vk::SampleCountFlagBits::e1,
		vk::Format::eR8Unorm,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false,  // not cubemap
		1       // single layer
	);

	// Transition to shader read-only initially (first frame outputs 1.0 via clear or fallback)
	m_shadow_mask_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader
	);
}

void ShadowMaskSystem::createComputeSetLayout() {
	// Set 1: compute I/O (depth input + shadow mask storage output)
	m_compute_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // depth
		.addBinding(1, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute) // shadow mask out
		.build();
}

void ShadowMaskSystem::createOutputSetLayout() {
	// Set 3: shadow mask output for PBR/simple shaders
	m_output_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment) // shadow mask
		.addBinding(1, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment)       // sampler
		.build();
}

void ShadowMaskSystem::createSampler() {
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eNearest,
		.addressModeU = vk::SamplerAddressMode::eClampToEdge,
		.addressModeV = vk::SamplerAddressMode::eClampToEdge,
		.addressModeW = vk::SamplerAddressMode::eClampToEdge,
		.mipLodBias = 0.0f,
		.anisotropyEnable = VK_FALSE,
		.compareEnable = VK_FALSE,
		.minLod = 0.0f,
		.maxLod = 0.0f,
		.borderColor = vk::BorderColor::eFloatOpaqueWhite,
		.unnormalizedCoordinates = VK_FALSE,
	};
	m_linear_clamp_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
}

void ShadowMaskSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout) {

	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(ShadowMaskPushConstant),
	};

	std::array<vk::DescriptorSetLayout, 3> set_layouts{
		*global_set_layout,
		*m_compute_set_layout->getDescriptorSetLayout(),
		*shadow_set_layout,
	};

	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void ShadowMaskSystem::createPipelines() {
	// Load SPIR-V for both non-MSAA and MSAA variants
	auto load_module = [&](const std::filesystem::path& path) {
		auto code = VeFileSystem::readFile(path);
		vk::ShaderModuleCreateInfo info{
			.codeSize = code.size(),
			.pCode = reinterpret_cast<const uint32_t*>(code.data()),
		};
		return vk::raii::ShaderModule(m_ve_device.getDevice(), info);
	};
	m_shader_module = load_module(m_shader_path / "shadow_mask_comp.spv");
	if (m_has_ms_support) {
		m_shader_module_ms = load_module(m_shader_path / "shadow_mask_ms_comp.spv");
	}

	// Create one pipeline per shadow mode × depth type (non-MSAA + MSAA)
	auto create_variants = [&](vk::raii::ShaderModule& module,
							   std::array<vk::raii::Pipeline, SHADOW_MODE_COUNT>& out) {
		for (uint32_t mode = 0; mode < SHADOW_MODE_COUNT; mode++) {
			std::array<uint32_t, 3> spec_data = {mode, m_pcf_samples, m_pcss_filter_samples};
			std::array<vk::SpecializationMapEntry, 3> spec_entries = {{
				{0, 0 * sizeof(uint32_t), sizeof(uint32_t)},
				{1, 1 * sizeof(uint32_t), sizeof(uint32_t)},
				{2, 2 * sizeof(uint32_t), sizeof(uint32_t)},
			}};
			vk::SpecializationInfo spec_info{
				.mapEntryCount = static_cast<uint32_t>(spec_entries.size()),
				.pMapEntries = spec_entries.data(),
				.dataSize = spec_data.size() * sizeof(uint32_t),
				.pData = spec_data.data(),
			};
			vk::PipelineShaderStageCreateInfo stage_info{
				.stage = vk::ShaderStageFlagBits::eCompute,
				.module = *module,
				.pName = "compMain",
				.pSpecializationInfo = &spec_info,
			};
			vk::ComputePipelineCreateInfo pipeline_info{
				.stage = stage_info,
				.layout = *m_pipeline_layout,
			};
			out[mode] = vk::raii::Pipeline(m_ve_device.getDevice(), nullptr, pipeline_info);
		}
	};
	create_variants(m_shader_module, m_pipelines);
	if (m_has_ms_support) {
		create_variants(m_shader_module_ms, m_pipelines_ms);
	}
}

void ShadowMaskSystem::createComputeUBOs(VeDescriptorPool& descriptor_pool,
	const vk::raii::DescriptorSetLayout& global_set_layout) {

	// Per-frame compute UBO buffers (same layout as global UBO, filled with prev-frame data)
	vk::DeviceSize ubo_size = sizeof(UniformBufferObject);
	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		m_compute_ubos[frame] = std::make_unique<VeBuffer>(
			m_ve_device,
			ubo_size,
			1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
		);
		m_compute_ubos[frame]->map();
	}

	// Dummy instance buffer for binding 1 of the global set layout (unused by compute shader)
	m_dummy_instance_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(InstanceData),
		1,
		vk::BufferUsageFlagBits::eStorageBuffer,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);

	// Create per-frame compute global descriptor sets using the same global_set_layout.
	// Allocate via VeDescriptorPool, then manually write the two bindings.
	auto dummy_instance_info = m_dummy_instance_buffer->getDescriptorInfo();
	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		auto ubo_info = m_compute_ubos[frame]->getDescriptorInfo();

		descriptor_pool.allocateDescriptor(global_set_layout, m_compute_global_descriptor_sets[frame]);

		// Write UBO (binding 0) and dummy SSBO (binding 1)
		std::array<vk::WriteDescriptorSet, 2> writes{
			vk::WriteDescriptorSet{
				.dstSet = *m_compute_global_descriptor_sets[frame],
				.dstBinding = 0,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = vk::DescriptorType::eUniformBuffer,
				.pBufferInfo = &ubo_info,
			},
			vk::WriteDescriptorSet{
				.dstSet = *m_compute_global_descriptor_sets[frame],
				.dstBinding = 1,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = vk::DescriptorType::eStorageBuffer,
				.pBufferInfo = &dummy_instance_info,
			},
		};
		m_ve_device.getDevice().updateDescriptorSets(writes, {});
	}
}

void ShadowMaskSystem::createDescriptorSets(VeDescriptorPool& descriptor_pool,
	const vk::raii::DescriptorSetLayout& /*global_set_layout*/) {
	// Shadow mask image info (for both compute storage and fragment sampled reads)
	vk::DescriptorImageInfo mask_storage_info{
		.imageView = *m_shadow_mask_image->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};

	vk::DescriptorImageInfo mask_sampled_info{
		.imageView = *m_shadow_mask_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};

	vk::DescriptorImageInfo sampler_info{
		.sampler = *m_linear_clamp_sampler,
	};

	// Depth image info
	vk::DescriptorImageInfo depth_info{
		.imageView = m_depth_image_view,
		.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	};

	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		// Compute I/O set (Set 1): depth input + shadow mask storage
		VeDescriptorWriter(*m_compute_set_layout, descriptor_pool)
			.writeImage(0, &depth_info)         // binding 0: depth texture
			.writeImage(1, &mask_storage_info)   // binding 1: shadow mask storage output
			.build(m_compute_descriptor_sets[frame]);

		// Output set (Set 3): shadow mask sampled + sampler (for PBR/simple)
		VeDescriptorWriter(*m_output_set_layout, descriptor_pool)
			.writeImage(0, &mask_sampled_info)   // binding 0: shadow mask sampled
			.writeImage(1, &sampler_info)         // binding 1: linear clamp sampler
			.build(m_output_descriptor_sets[frame]);
	}

	// Dummy output set: default white texture for when shadow mask is disabled
	vk::DescriptorImageInfo default_sampled_info{
		.imageView = *m_default_mask_texture->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	VeDescriptorWriter(*m_output_set_layout, descriptor_pool)
		.writeImage(0, &default_sampled_info)
		.writeImage(1, &sampler_info)
		.build(m_dummy_output_descriptor_set);
}

void ShadowMaskSystem::savePrevFrameUBO(const UniformBufferObject& ubo, uint32_t current_frame) {
	// On first call, use the current data so the first dispatch has reasonable matrices.
	if (!m_has_prev_data) {
		m_prev_ubo_data = ubo;
		m_has_prev_data = true;
	}
	// Write previous frame's UBO data to the CURRENT frame's compute buffer only.
	// The other frame's buffer may still be read by an in-flight compute dispatch.
	// The fence for current_frame guarantees its previous GPU work is complete.
	m_compute_ubos[current_frame]->writeToBuffer(const_cast<UniformBufferObject*>(&m_prev_ubo_data));
	// Save current frame data for use by next frame's dispatch
	m_prev_ubo_data = ubo;
}

void ShadowMaskSystem::dispatch(VeFrameInfo& frame_info) {
	auto& cmd = frame_info.compute_command_buffer;
	uint32_t frame = frame_info.current_frame;
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);

	// Barrier 1: depth buffer eDepthAttachmentOptimal → eDepthStencilReadOnlyOptimal
	// (make previous frame's depth writes visible to compute shader)
	vk::ImageMemoryBarrier2 depth_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
		.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_depth_image,
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
	};

	// Barrier 2: shadow mask eShaderReadOnlyOptimal → eGeneral for storage write
	vk::ImageMemoryBarrier2 mask_to_general{
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_shadow_mask_image->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};

	std::array<vk::ImageMemoryBarrier2, 2> pre_barriers = {depth_to_read, mask_to_general};
	vk::DependencyInfo pre_dep{
		.imageMemoryBarrierCount = static_cast<uint32_t>(pre_barriers.size()),
		.pImageMemoryBarriers = pre_barriers.data(),
	};
	cmd.pipelineBarrier2(pre_dep);

	// Bind pipeline and descriptor sets (using prev-frame compute UBO for Set 0)
	bool msaa = m_depth_sample_count != vk::SampleCountFlagBits::e1;
	auto& pipelines = msaa ? m_pipelines_ms : m_pipelines;
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[mode]);

	std::array<vk::DescriptorSet, 3> sets{
		*m_compute_global_descriptor_sets[frame],
		*m_compute_descriptor_sets[frame],
		*frame_info.shadow_descriptor_set,
	};
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout, 0, sets, {});

	// Push mask size + depth buffer size
	ShadowMaskPushConstant push{
		.mask_size = glm::vec2(static_cast<float>(m_extent.width), static_cast<float>(m_extent.height)),
		.depth_size = glm::vec2(static_cast<float>(m_depth_extent.width), static_cast<float>(m_depth_extent.height)),
	};
	cmd.pushConstants(
		*m_pipeline_layout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(ShadowMaskPushConstant), reinterpret_cast<const uint8_t*>(&push)));

	// Dispatch: 16x16 workgroups
	uint32_t groups_x = (m_extent.width + 15) / 16;
	uint32_t groups_y = (m_extent.height + 15) / 16;
	cmd.dispatch(groups_x, groups_y, 1);

	// Post-dispatch barriers: shadow mask to read-only + depth back to attachment
	vk::ImageMemoryBarrier2 mask_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_shadow_mask_image->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};

	vk::ImageMemoryBarrier2 depth_to_attachment{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_depth_image,
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
	};

	std::array<vk::ImageMemoryBarrier2, 2> post_barriers = {mask_to_read, depth_to_attachment};
	vk::DependencyInfo post_dep{
		.imageMemoryBarrierCount = static_cast<uint32_t>(post_barriers.size()),
		.pImageMemoryBarriers = post_barriers.data(),
	};
	cmd.pipelineBarrier2(post_dep);
}

void ShadowMaskSystem::recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D mask_extent,
	vk::Extent2D depth_extent, vk::SampleCountFlagBits depth_sample_count,
	const vk::raii::ImageView& depth_image_view, const vk::raii::Image& depth_image) {
	m_ve_device.getDevice().waitIdle();
	m_extent = mask_extent;
	m_depth_extent = depth_extent;
	m_depth_sample_count = depth_sample_count;
	m_depth_image = *depth_image;
	m_depth_image_view = *depth_image_view;
	createShadowMaskImage(mask_extent);
	// Re-create compute I/O and output descriptor sets (they reference the new shadow mask image)
	// but NOT the compute global descriptor sets (they only reference the UBO + dummy SSBO, which are unchanged)
	vk::DescriptorImageInfo mask_storage_info{
		.imageView = *m_shadow_mask_image->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo mask_sampled_info{
		.imageView = *m_shadow_mask_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo sampler_info{
		.sampler = *m_linear_clamp_sampler,
	};
	vk::DescriptorImageInfo depth_info{
		.imageView = m_depth_image_view,
		.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	};
	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		VeDescriptorWriter(*m_compute_set_layout, descriptor_pool)
			.writeImage(0, &depth_info)
			.writeImage(1, &mask_storage_info)
			.build(m_compute_descriptor_sets[frame]);
		VeDescriptorWriter(*m_output_set_layout, descriptor_pool)
			.writeImage(0, &mask_sampled_info)
			.writeImage(1, &sampler_info)
			.build(m_output_descriptor_sets[frame]);
	}
}

void ShadowMaskSystem::setShadowSamples(uint32_t pcf_samples, uint32_t pcss_filter_samples) {
	m_ve_device.getDevice().waitIdle();
	m_pcf_samples = pcf_samples;
	m_pcss_filter_samples = pcss_filter_samples;
	createPipelines();
}

} // namespace ve
