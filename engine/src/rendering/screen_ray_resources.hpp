/*
 * ScreenRayResources: what every screen-space ray march reads. The previous
 * frame's HDR radiance with its mip chain (the history) and the current
 * frame's min/max depth pyramid. SSR and RC bind both. RenderPipeline
 * recreates this before it broadcasts a ResolutionChangedEvent, generates
 * the pyramid once per frame before the first trace, and copies the history
 * after the transparent pass.
 */

#pragma once
#include "ve_export.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_descriptors.hpp"

#include <filesystem>
#include <memory>

namespace ve {

class MinMaxDepthPyramid;

class VENGINE_API ScreenRayResources {
public:
	ScreenRayResources(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		vk::Extent2D extent,
		vk::Format color_format,
		const vk::raii::ImageView& depth_image_view,
		const std::filesystem::path& shaders_dir);
	~ScreenRayResources();

	ScreenRayResources(const ScreenRayResources&) = delete;
	ScreenRayResources& operator=(const ScreenRayResources&) = delete;

	// Device idle. Consumers rebind afterwards.
	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D extent,
		vk::Format color_format, const vk::raii::ImageView& depth_image_view);

	// Pre: depth in eDepthStencilReadOnlyOptimal. Post: every pyramid mip in
	// eShaderReadOnlyOptimal.
	void generatePyramid(vk::raii::CommandBuffer& cmd);

	// Copies the resolve target into the history and rebuilds its mip chain.
	// Expects the resolve target in eShaderReadOnlyOptimal and returns both
	// images there. Graphics queue only.
	void recordHistoryCopy(vk::raii::CommandBuffer& command_buffer, vk::Image resolve_target);
	void invalidateHistory() { m_history_valid = false; }
	bool historyValid() const { return m_history_valid; }

	const vk::raii::ImageView& historyView() const { return m_history_image->getImageView(); }
	const vk::raii::ImageView& pyramidView() const;
	vk::Image pyramidImage() const;
	uint32_t pyramidMipLevels() const;
	vk::Extent2D pyramidMip0Extent() const;

private:
	void createHistoryImage();

	VeDevice& m_ve_device;
	vk::Extent2D m_extent{};
	vk::Format m_format{};

	std::unique_ptr<VeImage> m_history_image;
	std::unique_ptr<MinMaxDepthPyramid> m_pyramid;
	bool m_history_valid = false;
};

} // namespace ve
