#include "pch.hpp"
#include "rendering/shadow_mask_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
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
	const vk::raii::ImageView& depth_image_view,
	vk::Image depth_image)
	: m_ve_device(device), m_shader_path(std::move(shader_path)),
	  m_extent(mask_extent), m_depth_extent(depth_extent) {

	m_depth_image = depth_image;
	m_depth_image_view = *depth_image_view;
	m_default_mask_texture = resource_manager.load<VeTexture>("default_albedo");
	createShadowMaskImage(m_extent);
	createComputeSetLayout();
	createOutputSetLayout();
	createSampler();
	createPipelineLayout(global_set_layout, shadow_set_layout);
	createPipelines();
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
	auto load_module = [&](const std::filesystem::path& path) {
		auto code = VeFileSystem::readFile(path);
		vk::ShaderModuleCreateInfo info{
			.codeSize = code.size(),
			.pCode = reinterpret_cast<const uint32_t*>(code.data()),
		};
		return vk::raii::ShaderModule(m_ve_device.getDevice(), info);
	};
	m_shader_module = load_module(m_shader_path / "shadow_mask_comp.spv");

	// Create one pipeline per shadow mode (always single-sample resolved depth)
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
			.module = *m_shader_module,
			.pName = "compMain",
			.pSpecializationInfo = &spec_info,
		};
		vk::ComputePipelineCreateInfo pipeline_info{
			.stage = stage_info,
			.layout = *m_pipeline_layout,
		};
		m_pipelines[mode] = vk::raii::Pipeline(m_ve_device.getDevice(), nullptr, pipeline_info);
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

void ShadowMaskSystem::dispatch(VeFrameInfo& frame_info) {
	auto& cmd = frame_info.cmd();
	uint32_t frame = frame_info.current_frame;
	auto mode = static_cast<uint32_t>(frame_info.shadow_mode);

	// Pre-barrier: shadow mask eShaderReadOnlyOptimal -> eGeneral for storage write
	// Previous frame's PBR read is the src dependency.
	vk::ImageMemoryBarrier2 mask_to_general{
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_shadow_mask_image->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	vk::DependencyInfo pre_dep{
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &mask_to_general,
	};
	cmd.pipelineBarrier2(pre_dep);

	// Bind pipeline and descriptor sets (current-frame global UBO for Set 0)
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *m_pipelines[mode]);

	std::array<vk::DescriptorSet, 3> sets{
		*frame_info.global_descriptor_set,
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

	// Post-barrier: shadow mask to read-only for fragment shader consumption
	vk::ImageMemoryBarrier2 mask_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_shadow_mask_image->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	vk::DependencyInfo post_dep{
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &mask_to_read,
	};
	cmd.pipelineBarrier2(post_dep);
}

void ShadowMaskSystem::recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D mask_extent,
	vk::Extent2D depth_extent,
	const vk::raii::ImageView& depth_image_view, vk::Image depth_image) {
	// Precondition: device must be idle
	m_ve_device.assertDeviceIdle();
	m_extent = mask_extent;
	m_depth_extent = depth_extent;
	m_depth_image = depth_image;
	m_depth_image_view = *depth_image_view;
	createShadowMaskImage(mask_extent);
	// Re-create compute I/O and output descriptor sets (they reference the new shadow mask image)
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
	// Precondition: device must be idle
	m_ve_device.assertDeviceIdle();
	m_pcf_samples = pcf_samples;
	m_pcss_filter_samples = pcss_filter_samples;
	createPipelines();
}

} // namespace ve
