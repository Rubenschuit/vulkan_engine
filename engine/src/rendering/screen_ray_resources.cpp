#include "pch.hpp"
#include "rendering/screen_ray_resources.hpp"
#include "rendering/min_max_depth_pyramid.hpp"

#include <algorithm>
#include <cmath>

namespace ve {

ScreenRayResources::ScreenRayResources(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	vk::Extent2D extent,
	vk::Format color_format,
	const vk::raii::ImageView& depth_image_view,
	const std::filesystem::path& shaders_dir)
	: m_ve_device(device), m_extent(extent), m_format(color_format) {
	m_pyramid = std::make_unique<MinMaxDepthPyramid>(
		device, descriptor_pool, m_extent, depth_image_view, shaders_dir);
	createHistoryImage();
}

ScreenRayResources::~ScreenRayResources() = default;

void ScreenRayResources::recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D extent,
	vk::Format color_format, const vk::raii::ImageView& depth_image_view) {
	m_extent = extent;
	m_format = color_format;
	m_pyramid->recreate(descriptor_pool, extent, depth_image_view);
	createHistoryImage();
	m_history_valid = false;
}

void ScreenRayResources::createHistoryImage() {
	// Full mip chain
	uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(
		std::max(m_extent.width, m_extent.height)))) + 1u;
	m_history_image = std::make_unique<VeImage>(
		m_ve_device,
		m_extent.width,
		m_extent.height,
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
	m_history_image->setDebugName("Screen Ray History");
}

void ScreenRayResources::generatePyramid(vk::raii::CommandBuffer& cmd) {
	m_pyramid->generate(cmd);
}

const vk::raii::ImageView& ScreenRayResources::pyramidView() const {
	return m_pyramid->getPyramidView();
}

vk::Image ScreenRayResources::pyramidImage() const {
	return m_pyramid->getImage();
}

uint32_t ScreenRayResources::pyramidMipLevels() const {
	return m_pyramid->getMipLevels();
}

vk::Extent2D ScreenRayResources::pyramidMip0Extent() const {
	return m_pyramid->getMip0Extent();
}

void ScreenRayResources::recordHistoryCopy(vk::raii::CommandBuffer& command_buffer, vk::Image resolve_target) {
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
		.extent = {m_extent.width, m_extent.height, 1}
	};
	command_buffer.copyImage(
		resolve_target, vk::ImageLayout::eTransferSrcOptimal,
		m_history_image->getImage(), vk::ImageLayout::eTransferDstOptimal,
		region);

	// Build the mip chain: blit each level from the previous
	int32_t src_w = static_cast<int32_t>(m_extent.width);
	int32_t src_h = static_cast<int32_t>(m_extent.height);
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
