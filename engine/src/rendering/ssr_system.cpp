#include "pch.hpp"
#include "rendering/ssr_system.hpp"
#include "rendering/ssr_hiz_pyramid.hpp"
#include "vulkan/ve_buffer.hpp"
#include "platform/ve_file_system.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"
#include "utils/ve_log.hpp"

#include <algorithm>
#include <cmath>

namespace ve {

struct SsrPushConstant {
	glm::vec2 out_size;
	glm::vec2 depth_size;
	float proj_22;
	float proj_32;
	float thickness;       // view-space hit tolerance
	float max_roughness;   // no reflections above this roughness
	float max_distance;    // world-space ray length
	int max_steps;         // traversal iteration cap
	uint32_t hiz_mip_count;
	float _pad;
};

struct SsrResolvePush {
	glm::vec2 image_size;
	glm::vec2 depth_size;
	float proj_22;
	float proj_32;
	glm::vec2 _pad;
};

SsrSystem::SsrSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	std::filesystem::path shader_path,
	vk::Extent2D ssr_extent,
	vk::Extent2D full_extent,
	vk::Format color_format,
	const vk::raii::ImageView& depth_image_view,
	const vk::raii::ImageView& normal_roughness_image_view,
	EventBus& event_bus)
	: m_ve_device(device), m_shader_path(std::move(shader_path)),
	  m_ssr_extent(ssr_extent), m_full_extent(full_extent), m_format(color_format) {

	event_bus.subscribe<ResolutionChangedEvent>([this](const ResolutionChangedEvent& e) {
		m_full_extent = e.extent;
		m_format = e.offscreen_format;
		m_ssr_extent = e.ssr_half_res
			? vk::Extent2D{std::max(1u, e.extent.width / 2), std::max(1u, e.extent.height / 2)} : e.extent;
		m_depth_image_view = *e.depth_image_view;
		m_normal_image_view = *e.normal_roughness_image_view;
		m_hiz_pyramid->recreate(e.pool, e.extent, e.depth_image_view);
		createHistoryImage();
		createOutputImage();
		createResolvedImage();
		createDescriptorSets(e.pool);
		m_history_valid = false;
	});
	event_bus.subscribe<SsrResolutionChangedEvent>([this](const SsrResolutionChangedEvent& e) {
		m_ssr_extent = e.ssr_extent;
		createOutputImage();
		createResolvedImage();
		createDescriptorSets(e.pool);
	});
	event_bus.subscribe<SsrParametersChangedEvent>([this](const SsrParametersChangedEvent& e) {
		m_max_steps = e.max_steps;
		m_thickness = e.thickness;
		m_max_roughness = e.max_roughness;
		m_max_distance = e.max_distance;
	});

	m_depth_image_view = *depth_image_view;
	m_normal_image_view = *normal_roughness_image_view;
	m_hiz_pyramid = std::make_unique<SsrHizPyramid>(
		device, descriptor_pool, m_full_extent, depth_image_view, m_shader_path);
	createHistoryImage();
	createOutputImage();
	createResolvedImage();
	createDummyImage();
	createSetLayouts();
	createSampler();
	createPipeline(global_set_layout);
	createDescriptorSets(descriptor_pool);
}

SsrSystem::~SsrSystem() = default;

void SsrSystem::createHistoryImage() {
	// Full mip chain
	uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(
		std::max(m_full_extent.width, m_full_extent.height)))) + 1u;
	m_history_image = std::make_unique<VeImage>(
		m_ve_device,
		m_full_extent.width,
		m_full_extent.height,
		vk::SampleCountFlagBits::e1,
		m_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc
			| vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1, mips);
	m_history_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eComputeShader);
	m_history_image->setDebugName("SSR History");
}

void SsrSystem::createOutputImage() {
	m_output_image = std::make_unique<VeImage>(
		m_ve_device,
		m_ssr_extent.width,
		m_ssr_extent.height,
		vk::SampleCountFlagBits::e1,
		m_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1);
	m_output_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader);
	m_output_image->setDebugName("SSR Output");
}

void SsrSystem::createResolvedImage() {
	m_resolved_image = std::make_unique<VeImage>(
		m_ve_device,
		m_ssr_extent.width,
		m_ssr_extent.height,
		vk::SampleCountFlagBits::e1,
		m_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1);
	m_resolved_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader);
	m_resolved_image->setDebugName("SSR Resolved");
}

void SsrSystem::createDummyImage() {
	m_dummy_image = std::make_unique<VeImage>(
		m_ve_device, 4, 4,
		vk::SampleCountFlagBits::e1, vk::Format::eR8G8B8A8Unorm,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1, 1);

	constexpr uint32_t bytes = 4 * 4 * 4;
	std::vector<uint8_t> black(bytes, 0);
	VeBuffer staging(m_ve_device, bytes, 1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	staging.map();
	staging.writeToBuffer(black.data(), bytes);

	auto cmd = m_ve_device.beginSingleTimeCommands(QueueKind::Graphics);
	m_dummy_image->transitionImageLayout(*cmd,
		vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		{}, vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eTransfer);
	VeDevice::copyBufferToImage(*cmd, staging.getBuffer(), m_dummy_image->getImage(), 4, 4, 1);
	m_dummy_image->transitionImageLayout(*cmd,
		vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader);
	m_ve_device.endSingleTimeCommands(*cmd, QueueKind::Graphics);
	m_dummy_image->setDebugName("SSR Dummy");
}

void SsrSystem::createSetLayouts() {
	m_io_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // depth
		.addBinding(1, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // normal+roughness
		.addBinding(2, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // history color
		.addBinding(3, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eCompute)
		.addBinding(4, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute) // SSR out
		.addBinding(5, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // min/max depth pyramid
		.build();
	m_resolve_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // raw trace output
		.addBinding(1, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // depth
		.addBinding(2, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute) // resolved out
		.build();
	m_output_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment)
		.build();
}

void SsrSystem::createSampler() {
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eLinear,
		.addressModeU = vk::SamplerAddressMode::eClampToEdge,
		.addressModeV = vk::SamplerAddressMode::eClampToEdge,
		.addressModeW = vk::SamplerAddressMode::eClampToEdge,
		.mipLodBias = 0.0f,
		.anisotropyEnable = VK_FALSE,
		.compareEnable = VK_FALSE,
		.minLod = 0.0f,
		.maxLod = VK_LOD_CLAMP_NONE,
		.borderColor = vk::BorderColor::eFloatOpaqueWhite,
		.unnormalizedCoordinates = VK_FALSE,
	};
	m_linear_clamp_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
}

void SsrSystem::createPipeline(const vk::raii::DescriptorSetLayout& global_set_layout) {
	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(SsrPushConstant),
	};
	std::array<vk::DescriptorSetLayout, 2> set_layouts{
		*global_set_layout,
		*m_io_set_layout->getDescriptorSetLayout(),
	};
	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);

	auto code = VeFileSystem::readFile(m_shader_path / "ssr_trace_comp.spv");
	vk::ShaderModuleCreateInfo module_info{
		.codeSize = code.size(),
		.pCode = reinterpret_cast<const uint32_t*>(code.data()),
	};
	m_shader_module = vk::raii::ShaderModule(m_ve_device.getDevice(), module_info);

	vk::PipelineShaderStageCreateInfo stage_info{
		.stage = vk::ShaderStageFlagBits::eCompute,
		.module = *m_shader_module,
		.pName = "compMain",
	};
	vk::ComputePipelineCreateInfo pipeline_info{
		.stage = stage_info,
		.layout = *m_pipeline_layout,
	};
	m_pipeline = vk::raii::Pipeline(m_ve_device.getDevice(), nullptr, pipeline_info);

	vk::PushConstantRange resolve_push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(SsrResolvePush),
	};
	vk::DescriptorSetLayout resolve_set_layout = *m_resolve_set_layout->getDescriptorSetLayout();
	vk::PipelineLayoutCreateInfo resolve_layout_info{
		.setLayoutCount = 1,
		.pSetLayouts = &resolve_set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &resolve_push_range,
	};
	m_resolve_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), resolve_layout_info);

	auto resolve_code = VeFileSystem::readFile(m_shader_path / "ssr_resolve_comp.spv");
	vk::ShaderModuleCreateInfo resolve_module_info{
		.codeSize = resolve_code.size(),
		.pCode = reinterpret_cast<const uint32_t*>(resolve_code.data()),
	};
	m_resolve_shader_module = vk::raii::ShaderModule(m_ve_device.getDevice(), resolve_module_info);

	vk::PipelineShaderStageCreateInfo resolve_stage_info{
		.stage = vk::ShaderStageFlagBits::eCompute,
		.module = *m_resolve_shader_module,
		.pName = "compMain",
	};
	vk::ComputePipelineCreateInfo resolve_pipeline_info{
		.stage = resolve_stage_info,
		.layout = *m_resolve_pipeline_layout,
	};
	m_resolve_pipeline = vk::raii::Pipeline(m_ve_device.getDevice(), nullptr, resolve_pipeline_info);
}

void SsrSystem::createDescriptorSets(VeDescriptorPool& descriptor_pool) {
	vk::DescriptorImageInfo depth_info{
		.imageView = m_depth_image_view,
		.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	};
	vk::DescriptorImageInfo normal_info{
		.imageView = m_normal_image_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo history_info{
		.imageView = *m_history_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo sampler_info{
		.sampler = *m_linear_clamp_sampler,
	};
	vk::DescriptorImageInfo output_storage_info{
		.imageView = *m_output_image->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo raw_sampled_info{
		.imageView = *m_output_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo resolved_storage_info{
		.imageView = *m_resolved_image->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};
	bool resolve_active = m_ssr_extent.width != m_full_extent.width
		|| m_ssr_extent.height != m_full_extent.height;
	vk::DescriptorImageInfo output_sampled_info{
		.imageView = resolve_active ? *m_resolved_image->getImageView() : *m_output_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo dummy_info{
		.imageView = *m_dummy_image->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo pyramid_info{
		.imageView = *m_hiz_pyramid->getPyramidView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};

	VeDescriptorWriter(*m_io_set_layout, descriptor_pool)
		.writeImage(0, &depth_info)
		.writeImage(1, &normal_info)
		.writeImage(2, &history_info)
		.writeImage(3, &sampler_info)
		.writeImage(4, &output_storage_info)
		.writeImage(5, &pyramid_info)
		.build(m_io_descriptor_set);

	VeDescriptorWriter(*m_resolve_set_layout, descriptor_pool)
		.writeImage(0, &raw_sampled_info)
		.writeImage(1, &depth_info)
		.writeImage(2, &resolved_storage_info)
		.build(m_resolve_descriptor_set);

	VeDescriptorWriter(*m_output_set_layout, descriptor_pool)
		.writeImage(0, &output_sampled_info)
		.writeImage(1, &sampler_info)
		.build(m_output_descriptor_set);

	VeDescriptorWriter(*m_output_set_layout, descriptor_pool)
		.writeImage(0, &dummy_info)
		.writeImage(1, &sampler_info)
		.build(m_dummy_output_descriptor_set);
}

void SsrSystem::dispatch(VeFrameInfo& frame_info, vk::raii::CommandBuffer& cmd) {
	m_hiz_pyramid->generate(cmd);

	// The resolve only runs below full resolution, where it doubles as the
	// upsample prefilter; at full res the raw trace is sharper and we
	// sample it directly
	bool resolve_active = m_ssr_extent.width != m_full_extent.width
		|| m_ssr_extent.height != m_full_extent.height;

	// Raw output -> eGeneral for the trace, resolved -> eGeneral for the
	// resolve. Src stages cover both possible last readers (resolve compute
	// or PBR fragment) so half-res toggles stay correct.
	std::array<vk::ImageMemoryBarrier2, 2> to_general = {
		vk::ImageMemoryBarrier2{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
			.srcAccessMask = vk::AccessFlagBits2::eNone,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eGeneral,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_output_image->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
		},
		vk::ImageMemoryBarrier2{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
			.srcAccessMask = vk::AccessFlagBits2::eNone,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eGeneral,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_resolved_image->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
		}
	};
	vk::DependencyInfo dep_pre{
		.imageMemoryBarrierCount = resolve_active ? 2u : 1u,
		.pImageMemoryBarriers = to_general.data()};
	cmd.pipelineBarrier2(dep_pre);

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *m_pipeline);
	std::array<vk::DescriptorSet, 2> sets{
		*frame_info.global_descriptor_set,
		*m_io_descriptor_set,
	};
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout, 0, sets, {});

	const auto& proj = frame_info.camera_view.proj;
	SsrPushConstant push{
		.out_size = glm::vec2(static_cast<float>(m_ssr_extent.width), static_cast<float>(m_ssr_extent.height)),
		.depth_size = glm::vec2(static_cast<float>(m_full_extent.width), static_cast<float>(m_full_extent.height)),
		.proj_22 = proj[2][2],
		.proj_32 = proj[3][2],
		.thickness = m_thickness,
		.max_roughness = m_max_roughness,
		.max_distance = m_max_distance,
		.max_steps = m_max_steps,
		.hiz_mip_count = m_hiz_pyramid->getMipLevels(),
		._pad = 0.0f,
	};
	cmd.pushConstants(
		*m_pipeline_layout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(SsrPushConstant), reinterpret_cast<const uint8_t*>(&push)));

	uint32_t groups_x = (m_ssr_extent.width + 15) / 16;
	uint32_t groups_y = (m_ssr_extent.height + 15) / 16;
	cmd.dispatch(groups_x, groups_y, 1);

	// Raw output: eGeneral -> eShaderReadOnlyOptimal for its next reader
	vk::PipelineStageFlags2 raw_dst_stage = vk::PipelineStageFlagBits2::eFragmentShader;
	if (resolve_active)
		raw_dst_stage = vk::PipelineStageFlagBits2::eComputeShader;
	vk::ImageMemoryBarrier2 raw_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = raw_dst_stage,
		.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_output_image->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	vk::DependencyInfo dep_mid{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &raw_to_read};
	cmd.pipelineBarrier2(dep_mid);

	if (!resolve_active)
		return;

	// Resolve: confidence-weighted bilateral over the trace output
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *m_resolve_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_resolve_pipeline_layout,
		0, {*m_resolve_descriptor_set}, {});
	SsrResolvePush resolve_push{
		.image_size = glm::vec2(static_cast<float>(m_ssr_extent.width), static_cast<float>(m_ssr_extent.height)),
		.depth_size = glm::vec2(static_cast<float>(m_full_extent.width), static_cast<float>(m_full_extent.height)),
		.proj_22 = proj[2][2],
		.proj_32 = proj[3][2],
		._pad = glm::vec2(0.0f),
	};
	cmd.pushConstants(
		*m_resolve_pipeline_layout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(resolve_push), reinterpret_cast<const uint8_t*>(&resolve_push)));
	cmd.dispatch(groups_x, groups_y, 1);

	// Resolved: eGeneral -> eShaderReadOnlyOptimal for the PBR fragment stage
	vk::ImageMemoryBarrier2 resolved_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_resolved_image->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	vk::DependencyInfo dep_post{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &resolved_to_read};
	cmd.pipelineBarrier2(dep_post);
}

void SsrSystem::recordHistoryCopy(vk::raii::CommandBuffer& command_buffer, vk::Image resolve_target) {
	constexpr vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	uint32_t mips = m_history_image->getMipLevels();
	vk::ImageSubresourceRange history_all_mips{vk::ImageAspectFlagBits::eColor, 0, mips, 0, 1};

	// Transition resolve_target and all history mips for copy + mip blits
	std::array<vk::ImageMemoryBarrier2, 2> to_transfer = {
		vk::ImageMemoryBarrier2{
			.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
			.srcAccessMask = vk::AccessFlagBits2::eNone,
			.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.dstAccessMask = vk::AccessFlagBits2::eTransferRead,
			.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eTransferSrcOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = resolve_target,
			.subresourceRange = color_range
		},
		vk::ImageMemoryBarrier2{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
			.srcAccessMask = vk::AccessFlagBits2::eNone,
			.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
			.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eTransferDstOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_history_image->getImage(),
			.subresourceRange = history_all_mips
		}
	};
	vk::DependencyInfo to_transfer_dep = {
		.imageMemoryBarrierCount = static_cast<uint32_t>(to_transfer.size()),
		.pImageMemoryBarriers = to_transfer.data()
	};
	command_buffer.pipelineBarrier2(to_transfer_dep);

	vk::ImageCopy region{
		.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
		.srcOffset = {0, 0, 0},
		.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
		.dstOffset = {0, 0, 0},
		.extent = {m_full_extent.width, m_full_extent.height, 1}
	};
	command_buffer.copyImage(
		resolve_target, vk::ImageLayout::eTransferSrcOptimal,
		m_history_image->getImage(), vk::ImageLayout::eTransferDstOptimal,
		region);

	// Build the mip chain: blit each level from the previous
	int32_t src_w = static_cast<int32_t>(m_full_extent.width);
	int32_t src_h = static_cast<int32_t>(m_full_extent.height);
	for (uint32_t i = 1; i < mips; i++) {
		vk::ImageMemoryBarrier2 to_src{
			.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.dstAccessMask = vk::AccessFlagBits2::eTransferRead,
			.oldLayout = vk::ImageLayout::eTransferDstOptimal,
			.newLayout = vk::ImageLayout::eTransferSrcOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_history_image->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, i - 1, 1, 0, 1},
		};
		vk::DependencyInfo to_src_dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_src};
		command_buffer.pipelineBarrier2(to_src_dep);

		int32_t dst_w = std::max(src_w / 2, 1);
		int32_t dst_h = std::max(src_h / 2, 1);
		vk::ImageBlit blit{
			.srcSubresource = {vk::ImageAspectFlagBits::eColor, i - 1, 0, 1},
			.srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0}, vk::Offset3D{src_w, src_h, 1}},
			.dstSubresource = {vk::ImageAspectFlagBits::eColor, i, 0, 1},
			.dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0}, vk::Offset3D{dst_w, dst_h, 1}},
		};
		command_buffer.blitImage(
			m_history_image->getImage(), vk::ImageLayout::eTransferSrcOptimal,
			m_history_image->getImage(), vk::ImageLayout::eTransferDstOptimal,
			blit, vk::Filter::eLinear);
		src_w = dst_w;
		src_h = dst_h;
	}

	// Back to shader read: after the blit loop mips [0, mips-1) sit in
	// TransferSrc and the last mip in TransferDst
	std::array<vk::ImageMemoryBarrier2, 3> from_transfer = {
		vk::ImageMemoryBarrier2{
			.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.srcAccessMask = vk::AccessFlagBits2::eNone,
			.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
			.oldLayout = vk::ImageLayout::eTransferSrcOptimal,
			.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = resolve_target,
			.subresourceRange = color_range
		},
		vk::ImageMemoryBarrier2{
			.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
			.oldLayout = vk::ImageLayout::eTransferDstOptimal,
			.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_history_image->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, mips - 1, 1, 0, 1}
		},
		vk::ImageMemoryBarrier2{
			.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.srcAccessMask = vk::AccessFlagBits2::eNone,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
			.oldLayout = vk::ImageLayout::eTransferSrcOptimal,
			.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_history_image->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mips - 1, 0, 1}
		}
	};
	vk::DependencyInfo from_transfer_dep = {
		.imageMemoryBarrierCount = (mips > 1) ? 3u : 2u,
		.pImageMemoryBarriers = from_transfer.data()
	};
	command_buffer.pipelineBarrier2(from_transfer_dep);

	m_history_valid = true;
}

} // namespace ve
