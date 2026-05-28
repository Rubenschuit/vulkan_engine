#include "pch.hpp"
#include "rendering/hiz_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_debug_utils.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "utils/ve_log.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"

#include <cmath>
#include <cstring>

namespace ve {

struct SpdConstants {
	uint32_t   mips;
	uint32_t   numWorkGroups;
	glm::vec2  invInputSize;
	glm::vec2  workGroupOffset;
	glm::uvec2 srcSize;
	glm::uvec2 mip5Extent;
};

static uint32_t nextPow2(uint32_t v) {
	if (v <= 1)
		return 1;
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return v + 1;
}

// Mirror of SPD's SpdSetup() helper. Returns dispatch thread group counts and
// the number of mips to write. We always downsample the full image starting at
// (0, 0), so workGroupOffset is implicitly 0.
struct SpdDispatch {
	uint32_t group_count_x;
	uint32_t group_count_y;
	uint32_t num_work_groups;
	uint32_t mip_count;
};

static SpdDispatch spdSetup(uint32_t src_w, uint32_t src_h, uint32_t max_mips) {
	SpdDispatch out{};
	out.group_count_x = (src_w + 63u) / 64u;
	out.group_count_y = (src_h + 63u) / 64u;
	out.num_work_groups = out.group_count_x * out.group_count_y;
	uint32_t resolution = std::max(src_w, src_h);
	uint32_t derived = static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(resolution))));
	out.mip_count = std::min(derived, max_mips);
	return out;
}

HizSystem::HizSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	vk::Extent2D extent,
	const vk::raii::ImageView& depth_image_view,
	vk::Image depth_image,
	const std::filesystem::path& shaders_dir,
	EventBus& event_bus)
	: m_ve_device(device), m_event_bus(&event_bus) {

	m_resolution_sub = event_bus.subscribe<ResolutionChangedEvent>(
		[this](const ResolutionChangedEvent& e) {
			recreate(e.pool, e.extent, e.depth_image_view, e.depth_image);
		});

	m_depth_image = depth_image;
	m_depth_image_view = *depth_image_view;

	createHizImages(extent);
	createMipViews();
	createSampler();
	createComputeSetLayout();
	createPipelineLayout();
	createPipeline(shaders_dir);
	createAtomicCounterBuffers();
	createDescriptorSets(descriptor_pool);

	VE_LOGI("HizSystem: SPD (screen " << m_screen_width << "x" << m_screen_height
	         << ", padded " << m_width << "x" << m_height
	         << ", " << m_mip_levels << " mips)");
}

HizSystem::~HizSystem() {
	if (m_event_bus && m_resolution_sub != 0)
		m_event_bus->unsubscribe<ResolutionChangedEvent>(m_resolution_sub);
}

void HizSystem::createHizImages(vk::Extent2D extent) {
	m_screen_width = extent.width;
	m_screen_height = extent.height;
	// Power of two padding: keeps every mip an exact 2x reduction of
	// the previous, so no odd-dimension drops accumulate at the boundary.
	m_padded_source_width = nextPow2(extent.width);
	m_padded_source_height = nextPow2(extent.height);
	m_width = m_padded_source_width / 2;
	m_height = m_padded_source_height / 2;
	uint32_t max_mips_dim = static_cast<uint32_t>(std::floor(std::log2(std::max(m_width, m_height)))) + 1u;
	m_mip_levels = std::min({max_mips_dim, static_cast<uint32_t>(MAX_HIZ_MIPS), SPD_MAX_MIPS});

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
		m_hiz_images[i]->setDebugName(("Hi-Z [" + std::to_string(i) + "]").c_str());
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
			auto name = "Hi-Z Mip " + std::to_string(mip) + " [" + std::to_string(f) + "]";
			setDebugName(m_ve_device, m_hiz_mip_views[f].back(), name.c_str());
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

void HizSystem::createComputeSetLayout() {
	m_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eCompute)
		.addBinding(2, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute, SPD_MAX_MIPS)
		.addBinding(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.build();
}

void HizSystem::createPipelineLayout() {
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

void HizSystem::createPipeline(const std::filesystem::path& shaders_dir) {
	m_compute_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, shaders_dir / "hiz_spd_comp.spv", m_pipeline_layout);
}

void HizSystem::createAtomicCounterBuffers() {
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
		m_atomic_counter_buffers[f] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(uint32_t), 1,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_atomic_counter_buffers[f]->setDebugName(
			("Hi-Z SPD Atomic Counter [" + std::to_string(f) + "]").c_str());

		// SPD resets the counter back to 0 at the end of every dispatch, so
		// zero-init at creation is sufficient.
		uint32_t zero = 0;
		m_atomic_counter_buffers[f]->map();
		m_atomic_counter_buffers[f]->writeToBuffer(&zero, sizeof(uint32_t));
		m_atomic_counter_buffers[f]->unmap();
	}
}

void HizSystem::createDescriptorSets(VeDescriptorPool& pool) {
	vk::DescriptorImageInfo sampler_info{.sampler = *m_nearest_sampler};

	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
		vk::DescriptorImageInfo src_info{
			.imageView = m_depth_image_view,
			.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		};

		// SPD requires SPD_MAX_MIPS (12) storage image entries even when
		// fewer mips exist. Pad the unused tail with mip 0's view — the shader
		// will not write past `mips` in the push constants.
		std::array<vk::DescriptorImageInfo, SPD_MAX_MIPS> mip_infos{};
		for (uint32_t i = 0; i < SPD_MAX_MIPS; i++) {
			uint32_t bind_mip = (i < m_mip_levels) ? i : 0u;
			mip_infos[i] = {
				.imageView = *m_hiz_mip_views[f][bind_mip],
				.imageLayout = vk::ImageLayout::eGeneral,
			};
		}

		vk::DescriptorBufferInfo counter_info =
			m_atomic_counter_buffers[f]->getDescriptorInfo();

		VeDescriptorWriter(*m_set_layout, pool)
			.writeImage(0, &src_info)
			.writeImage(1, &sampler_info)
			.writeImageArray(2, mip_infos.data(), SPD_MAX_MIPS)
			.writeBuffer(3, &counter_info)
			.build(m_descriptor_sets[f]);
	}
}

void HizSystem::generate(vk::raii::CommandBuffer& cmd, uint32_t frame_index) {
	vk::Image hiz_image = m_hiz_images[frame_index]->getImage();

	// Transition all Hi-Z mips to eGeneral for storage writes.
	vk::ImageMemoryBarrier2 to_general{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite
			| vk::AccessFlagBits2::eShaderSampledRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite
			| vk::AccessFlagBits2::eShaderStorageRead,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = hiz_image,
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, m_mip_levels, 0, 1},
	};
	vk::DependencyInfo dep_in{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general};
	cmd.pipelineBarrier2(dep_in);

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout,
		0, {*m_descriptor_sets[frame_index]}, {});

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

	// Final barrier: all Hi-Z mips eGeneral -> eShaderReadOnlyOptimal.
	vk::ImageMemoryBarrier2 to_sampled{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = hiz_image,
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, m_mip_levels, 0, 1},
	};
	vk::DependencyInfo dep_out{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_sampled};
	cmd.pipelineBarrier2(dep_out);
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
