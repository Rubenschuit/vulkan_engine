#include "pch.hpp"
#include "rendering/hiz_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "utils/ve_log.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

#include <cmath>

namespace ve {

struct HizPushConstants {
	glm::uvec2 dst_size;
	glm::vec2 src_texel_size;
	uint32_t mip_count;
	uint32_t write_source;
};

static constexpr uint32_t MIPS_PASS1 = 6; // 1 copy + 5 reductions
static constexpr uint32_t MIPS_TAIL = 5;  // 5 reductions per subsequent pass

HizSystem::HizSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	vk::Extent2D extent,
	const vk::raii::ImageView& depth_image_view,
	vk::Image depth_image,
	const std::filesystem::path& shaders_dir,
	EventBus& event_bus)
	: m_ve_device(device) {

	event_bus.subscribe<ResolutionChangedEvent>([this](const ResolutionChangedEvent& e) {
		recreate(e.pool, e.extent, e.depth_image_view, e.depth_image);
	});

	m_depth_image = depth_image;
	m_depth_image_view = *depth_image_view;

	createHizImages(extent);
	createMipViews();
	createSampler();
	createDummyImage();
	createComputeSetLayout();
	createPipelineLayout();
	createPipeline(shaders_dir);
	createDescriptorSets(descriptor_pool);

	VE_LOGI("HizSystem: initialized (" << m_width << "x" << m_height << ", "
	         << m_mip_levels << " mips, " << m_pass_count << " passes)");
}

HizSystem::~HizSystem() = default;

void HizSystem::createHizImages(vk::Extent2D extent) {
	m_width = extent.width;
	m_height = extent.height;
	m_mip_levels = static_cast<uint32_t>(std::floor(std::log2(std::max(m_width, m_height)))) + 1;
	if (m_mip_levels > MAX_HIZ_MIPS)
		m_mip_levels = MAX_HIZ_MIPS;

	if (m_mip_levels <= MIPS_PASS1)
		m_pass_count = 1;
	else if (m_mip_levels <= MIPS_PASS1 + MIPS_TAIL)
		m_pass_count = 2;
	else
		m_pass_count = 3;

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_hiz_images[i] = std::make_unique<VeImage>(
			m_ve_device,
			m_width, m_height,
			vk::SampleCountFlagBits::e1,
			vk::Format::eR32Sfloat,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false, 1,
			m_mip_levels);

		m_hiz_images[i]->transitionImageLayout(
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eNone,
			vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eComputeShader);
	}
}

void HizSystem::createMipViews() {
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
		m_hiz_mip_views[f].clear();
		m_hiz_mip_views[f].reserve(m_mip_levels);

		for (uint32_t mip = 0; mip < m_mip_levels; mip++) {
			vk::ImageViewCreateInfo view_info{
				.image = m_hiz_images[f]->getImage(),
				.viewType = vk::ImageViewType::e2D,
				.format = vk::Format::eR32Sfloat,
				.subresourceRange = {
					.aspectMask = vk::ImageAspectFlagBits::eColor,
					.baseMipLevel = mip,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
			};
			m_hiz_mip_views[f].emplace_back(m_ve_device.getDevice(), view_info);
		}
	}
}

void HizSystem::createSampler() {
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
	m_nearest_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
}

void HizSystem::createDummyImage() {
	m_dummy_image = std::make_unique<VeImage>(
		m_ve_device, 1, 1,
		vk::SampleCountFlagBits::e1,
		vk::Format::eR32Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1);
	m_dummy_image->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eGeneral,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eShaderStorageWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eComputeShader);
}

void HizSystem::createComputeSetLayout() {
	auto b = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eCompute);
	for (uint32_t i = 2; i <= 7; i++)
		b.addBinding(i, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute);
	m_downsample_set_layout = b.build();
}

void HizSystem::createPipelineLayout() {
	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(HizPushConstants),
	};

	std::array<vk::DescriptorSetLayout, 1> set_layouts{
		*m_downsample_set_layout->getDescriptorSetLayout(),
	};

	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void HizSystem::createPipeline(const std::filesystem::path& shaders_dir) {
	m_compute_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, shaders_dir / "hiz_build_comp.spv", m_pipeline_layout);
}

void HizSystem::createDescriptorSets(VeDescriptorPool& pool) {
	vk::DescriptorImageInfo sampler_info{.sampler = *m_nearest_sampler};
	vk::DescriptorImageInfo dummy_info{
		.imageView = *m_dummy_image->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};

	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
		m_pass_sets[f].clear();
		m_pass_sets[f].reserve(m_pass_count);

		for (uint32_t pass = 0; pass < m_pass_count; pass++) {
			// Source: depth buffer for pass 0, previous pass's last mip for pass 1+
			vk::DescriptorImageInfo src_info;
			if (pass == 0) {
				src_info = {
					.imageView = m_depth_image_view,
					.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
				};
			} else {
				uint32_t src_mip = (pass == 1) ? 5 : 10;
				src_info = {
					.imageView = *m_hiz_mip_views[f][src_mip],
					.imageLayout = vk::ImageLayout::eGeneral,
				};
			}

			// Output mip range for this pass
			uint32_t first_out = (pass == 0) ? 0 : (pass == 1) ? 6 : 11;
			vk::DescriptorImageInfo out_infos[6];
			for (uint32_t i = 0; i < 6; i++) {
				uint32_t mip = first_out + i;
				if (mip < m_mip_levels)
					out_infos[i] = {
						.imageView = *m_hiz_mip_views[f][mip],
						.imageLayout = vk::ImageLayout::eGeneral,
					};
				else
					out_infos[i] = dummy_info;
			}

			vk::raii::DescriptorSet set{nullptr};
			VeDescriptorWriter(*m_downsample_set_layout, pool)
				.writeImage(0, &src_info)
				.writeImage(1, &sampler_info)
				.writeImage(2, &out_infos[0])
				.writeImage(3, &out_infos[1])
				.writeImage(4, &out_infos[2])
				.writeImage(5, &out_infos[3])
				.writeImage(6, &out_infos[4])
				.writeImage(7, &out_infos[5])
				.build(set);
			m_pass_sets[f].push_back(std::move(set));
		}
	}
}

void HizSystem::generate(vk::raii::CommandBuffer& cmd, uint32_t frame_index) {
	vk::Image hiz_image = m_hiz_images[frame_index]->getImage();

	// Transition all Hi-Z mips to eGeneral for storage writes
	{
		vk::ImageMemoryBarrier2 barrier{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eGeneral,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = hiz_image,
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, m_mip_levels, 0, 1},
		};
		vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
		cmd.pipelineBarrier2(dep);
	}

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());

	for (uint32_t pass = 0; pass < m_pass_count; pass++) {
		// Between-pass barrier: sync storage writes on source mip before reading
		if (pass > 0) {
			uint32_t src_mip = (pass == 1) ? 5 : 10;
			vk::ImageMemoryBarrier2 sync{
				.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
				.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
				.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
				.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
				.oldLayout = vk::ImageLayout::eGeneral,
				.newLayout = vk::ImageLayout::eGeneral,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = hiz_image,
				.subresourceRange = {vk::ImageAspectFlagBits::eColor, src_mip, 1, 0, 1},
			};
			vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &sync};
			cmd.pipelineBarrier2(dep);
		}

		// Compute pass parameters
		uint32_t first_out = (pass == 0) ? 0 : (pass == 1) ? 6 : 11;
		uint32_t remaining = m_mip_levels - first_out;
		uint32_t pass_mip_count = (pass == 0) ? std::min(MIPS_PASS1, remaining)
		                                      : std::min(MIPS_TAIL, remaining);

		// dst_size = region loaded into LDS (full res for pass 0, source mip size for pass 1+)
		uint32_t dst_w, dst_h, src_w, src_h;
		if (pass == 0) {
			dst_w = m_width;
			dst_h = m_height;
			src_w = m_width;
			src_h = m_height;
		} else {
			uint32_t src_mip = (pass == 1) ? 5 : 10;
			dst_w = std::max(1u, m_width >> src_mip);
			dst_h = std::max(1u, m_height >> src_mip);
			src_w = dst_w;
			src_h = dst_h;
		}

		HizPushConstants pc{
			.dst_size = glm::uvec2(dst_w, dst_h),
			.src_texel_size = glm::vec2(1.0f / static_cast<float>(src_w),
			                            1.0f / static_cast<float>(src_h)),
			.mip_count = pass_mip_count,
			.write_source = (pass == 0) ? 1u : 0u,
		};
		cmd.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eCompute,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(pc), reinterpret_cast<const uint8_t*>(&pc)));

		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout,
			0, {*m_pass_sets[frame_index][pass]}, {});

		uint32_t groups_x = (dst_w + 31) / 32;
		uint32_t groups_y = (dst_h + 31) / 32;
		cmd.dispatch(groups_x, groups_y, 1);
	}

	// Final barrier: all Hi-Z mips eGeneral -> eShaderReadOnlyOptimal
	{
		vk::ImageMemoryBarrier2 barrier{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
			.oldLayout = vk::ImageLayout::eGeneral,
			.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = hiz_image,
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, m_mip_levels, 0, 1},
		};
		vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
		cmd.pipelineBarrier2(dep);
	}
}

void HizSystem::recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D extent,
                          const vk::raii::ImageView& depth_image_view,
                          vk::Image depth_image) {
	m_ve_device.assertDeviceIdle();
	m_depth_image = depth_image;
	m_depth_image_view = *depth_image_view;
	createHizImages(extent);
	createMipViews();
	createDescriptorSets(descriptor_pool);
}

const vk::raii::ImageView& HizSystem::getHizImageView(uint32_t frame) const {
	return m_hiz_images[frame]->getImageView();
}

vk::Image HizSystem::getHizImage(uint32_t frame) const {
	return m_hiz_images[frame]->getImage();
}

} // namespace ve
