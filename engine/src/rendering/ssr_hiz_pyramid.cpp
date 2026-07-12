#include "pch.hpp"
#include "rendering/ssr_hiz_pyramid.hpp"
#include "rendering/spd_util.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_debug_utils.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "utils/ve_log.hpp"

#include <cmath>

namespace ve {

SsrHizPyramid::SsrHizPyramid(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	vk::Extent2D depth_extent,
	const vk::raii::ImageView& depth_image_view,
	const std::filesystem::path& shaders_dir)
	: m_ve_device(device) {

	m_depth_image_view = *depth_image_view;

	createImage(depth_extent);
	createMipViews();
	createSampler();
	createComputeSetLayout();
	createPipelineLayout();
	createPipeline(shaders_dir);
	createAtomicCounterBuffer();
	createDescriptorSet(descriptor_pool);

	VE_LOGI("SsrHizPyramid: SPD (screen " << m_screen_width << "x" << m_screen_height
	         << ", padded " << m_width << "x" << m_height
	         << ", " << m_mip_levels << " mips)");
}

SsrHizPyramid::~SsrHizPyramid() = default;

void SsrHizPyramid::createImage(vk::Extent2D depth_extent) {
	m_screen_width = depth_extent.width;
	m_screen_height = depth_extent.height;
	m_padded_source_width = nextPow2(depth_extent.width);
	m_padded_source_height = nextPow2(depth_extent.height);
	m_width = m_padded_source_width / 2;
	m_height = m_padded_source_height / 2;
	uint32_t max_mips_dim = static_cast<uint32_t>(std::floor(std::log2(std::max(m_width, m_height)))) + 1u;
	m_mip_levels = std::min({max_mips_dim, static_cast<uint32_t>(MAX_HIZ_MIPS), SPD_MAX_MIPS});

	m_image = std::make_unique<VeImage>(
		m_ve_device,
		m_width, m_height,
		vk::SampleCountFlagBits::e1,
		vk::Format::eR32G32Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1,
		m_mip_levels);

	m_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eComputeShader);
	m_image->setDebugName("SSR Hi-Z");
}

void SsrHizPyramid::createMipViews() {
	m_mip_views.clear();
	m_mip_views.reserve(m_mip_levels);

	for (uint32_t mip = 0; mip < m_mip_levels; mip++) {
		vk::ImageViewCreateInfo view_info{
			.image = m_image->getImage(),
			.viewType = vk::ImageViewType::e2D,
			.format = vk::Format::eR32G32Sfloat,
			.subresourceRange = {
				.aspectMask = vk::ImageAspectFlagBits::eColor,
				.baseMipLevel = mip,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		m_mip_views.emplace_back(m_ve_device.getDevice(), view_info);
		auto name = "SSR Hi-Z Mip " + std::to_string(mip);
		setDebugName(m_ve_device, m_mip_views.back(), name.c_str());
	}
}

void SsrHizPyramid::createSampler() {
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eNearest,
		.minFilter = vk::Filter::eNearest,
		.mipmapMode = vk::SamplerMipmapMode::eNearest,
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
	m_point_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
}

void SsrHizPyramid::createComputeSetLayout() {
	m_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eCompute)
		.addBinding(2, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute, SPD_MAX_MIPS)
		.addBinding(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.build();
}

void SsrHizPyramid::createPipelineLayout() {
	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(SpdConstants),
	};

	std::array<vk::DescriptorSetLayout, 1> set_layouts{
		*m_set_layout->getDescriptorSetLayout(),
	};

	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void SsrHizPyramid::createPipeline(const std::filesystem::path& shaders_dir) {
	m_compute_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, shaders_dir / "ssr_hiz_spd_comp.spv", m_pipeline_layout);
}

void SsrHizPyramid::createAtomicCounterBuffer() {
	m_atomic_counter_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(uint32_t), 1,
		vk::BufferUsageFlagBits::eStorageBuffer,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	m_atomic_counter_buffer->setDebugName("SSR Hi-Z SPD Atomic Counter");

	uint32_t zero = 0;
	m_atomic_counter_buffer->map();
	m_atomic_counter_buffer->writeToBuffer(&zero, sizeof(uint32_t));
	m_atomic_counter_buffer->unmap();
}

void SsrHizPyramid::createDescriptorSet(VeDescriptorPool& pool) {
	vk::DescriptorImageInfo sampler_info{.sampler = *m_point_sampler};

	vk::DescriptorImageInfo src_info{
		.imageView = m_depth_image_view,
		.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	};

	// SPD requires SPD_MAX_MIPS (12) storage image entries even when
	// fewer mips exist
	std::array<vk::DescriptorImageInfo, SPD_MAX_MIPS> mip_infos{};
	for (uint32_t i = 0; i < SPD_MAX_MIPS; i++) {
		uint32_t bind_mip = (i < m_mip_levels) ? i : 0u;
		mip_infos[i] = {
			.imageView = *m_mip_views[bind_mip],
			.imageLayout = vk::ImageLayout::eGeneral,
		};
	}

	vk::DescriptorBufferInfo counter_info =
		m_atomic_counter_buffer->getDescriptorInfo();

	VeDescriptorWriter(*m_set_layout, pool)
		.writeImage(0, &src_info)
		.writeImage(1, &sampler_info)
		.writeImageArray(2, mip_infos.data(), SPD_MAX_MIPS)
		.writeBuffer(3, &counter_info)
		.build(m_descriptor_set);
}

void SsrHizPyramid::generate(vk::raii::CommandBuffer& cmd) {
	// All mips to eGeneral for storage writes. This is a write-after-read
	// against the previous frame's trace
	vk::ImageMemoryBarrier2 to_general{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite
			| vk::AccessFlagBits2::eShaderStorageRead,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_image->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, m_mip_levels, 0, 1},
	};
	vk::DependencyInfo dep_in{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general};
	cmd.pipelineBarrier2(dep_in);

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout,
		0, {*m_descriptor_set}, {});

	SpdDispatch d = spdSetup(m_padded_source_width, m_padded_source_height, m_mip_levels);

	SpdConstants pc{
		.mips = d.mip_count,
		.numWorkGroups = d.num_work_groups,
		.invInputSize = glm::vec2(
			1.0f / static_cast<float>(m_screen_width),
			1.0f / static_cast<float>(m_screen_height)),
		.workGroupOffset = glm::vec2(0.0f, 0.0f),
		.srcSize = glm::uvec2(m_screen_width, m_screen_height),
		.mip5Extent = glm::uvec2(d.group_count_x, d.group_count_y),
	};
	cmd.pushConstants(
		*m_pipeline_layout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(pc), reinterpret_cast<const uint8_t*>(&pc)));

	cmd.dispatch(d.group_count_x, d.group_count_y, 1);

	// All mips eGeneral -> eShaderReadOnlyOptimal for the trace
	vk::ImageMemoryBarrier2 to_sampled{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_image->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, m_mip_levels, 0, 1},
	};
	vk::DependencyInfo dep_out{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_sampled};
	cmd.pipelineBarrier2(dep_out);
}

void SsrHizPyramid::recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D depth_extent,
                              const vk::raii::ImageView& depth_image_view) {
	m_ve_device.assertDeviceIdle();
	m_depth_image_view = *depth_image_view;
	createImage(depth_extent);
	createMipViews();
	createDescriptorSet(descriptor_pool);
}

const vk::raii::ImageView& SsrHizPyramid::getPyramidView() const {
	return m_image->getImageView();
}

}