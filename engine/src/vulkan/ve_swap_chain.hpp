/* VeSwapChain is owned by VeRenderer and is responsible for managing the swap chain
 and its associated resources. This includes image views, depth resources and
synchronization objects. Also sets the number of samples for MSAA.
 *
 * Frame submission (timeline semaphore = m_frame_timeline):
 *   Scene frame:
 *     compute -> pre_swap_graphics -> [optional: shadow_graphics -> depth_compute]
 *             -> swap_graphics + present
 *   No-scene frame (editor UI only): submitUIOnly (no timeline involvement).
 *
 *   The two compute submits are distinguished by their data dependency:
 *   - compute: skinning, particles, cluster lights. No graphics-output dependency, so
 *     submits at frame start.
 *   - depth_compute: GTAO and Hi-Z. Reads the depth buffer produced by pre_swap_graphics,
 *     so submits after it. Only used when a dedicated compute queue family is available
 *     (compute-capable but not graphics-capable); otherwise this work runs inline in
 *     pre_swap_graphics on the single graphics+compute queue.
 *
 *   Graphics CBs:
 *   - pre_swap_graphics: work that doesn't touch the swap image (culling, depth prepass,
 *     and without a dedicated compute queue also shadows / shadow mask / GTAO / Hi-Z).
 *     Submitted early so the GPU can start before the CPU blocks on acquireNextImage.
 *   - swap_graphics: final composition (scene render, bloom, post-process). Waits on the
 *     image_available binary semaphore.
 *
 * Graphics-side timeline waits gate at
 *   eDrawIndirect | eVertexInput | eFragmentShader
 */
#pragma once
#include "ve_export.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <ve_config.hpp>

#include <vector>
#include <memory>

namespace ve {

class VENGINE_API VeSwapChain {
public:
	VeSwapChain(VeDevice& device, vk::Extent2D window_extent, vk::SampleCountFlagBits desired_num_samples, vk::PresentModeKHR present_mode, bool hdr_enabled);
	VeSwapChain(VeDevice& device, vk::Extent2D window_extent, vk::SampleCountFlagBits desired_num_samples, vk::PresentModeKHR present_mode, bool hdr_enabled, std::shared_ptr<VeSwapChain> old_swap_chain);
	~VeSwapChain();

	// Not copyable or movable
	VeSwapChain(const VeSwapChain&) = delete;
	VeSwapChain& operator=(const VeSwapChain&) = delete;

	// Getters
	uint32_t getWidth() const { return m_swap_chain_extent.width; }
	uint32_t getHeight() const { return m_swap_chain_extent.height; }
	size_t getImageCount() const { return m_swap_chain_images.size(); }
	uint32_t getCurrentFrame() const { return m_current_frame; }
	vk::raii::SwapchainKHR& getSwapChain() { return m_swap_chain; }
	vk::Format getSwapChainImageFormat() const { return m_swap_chain_image_format; }
	vk::ColorSpaceKHR getSwapChainColorSpace() const { return m_surface_format.colorSpace; }
	vk::Format getOffscreenImageFormat() const { return m_offscreen_image_format; }
	vk::Extent2D getSwapChainExtent() const { return m_swap_chain_extent; }
	vk::SampleCountFlagBits getSwapChainSampleCount() const { return m_desired_num_samples; }
	vk::PresentModeKHR getPresentMode() const { return m_present_mode; }
	const vk::raii::ImageView& getImageView(size_t index) const { return m_swap_chain_image_views[index]; };
	const vk::raii::ImageView& getColorImageView() const { return m_color_image->getImageView(); }
	const vk::raii::ImageView& getResolveTargetImageView() const { return m_resolve_target_image->getImageView(); }
	const vk::raii::ImageView& getDepthImageView() const { return m_depth_image->getImageView(); }
	vk::Image getDepthImage() const { return m_depth_image->getImage(); }
	/// Single-sample depth (resolved from MSAA prepass, or same as depth when no MSAA)
	const vk::raii::ImageView& getResolvedDepthImageView() const {
		return m_resolved_depth_image ? m_resolved_depth_image->getImageView() : m_depth_image->getImageView();
	}
	vk::Image getResolvedDepthImage() const {
		return m_resolved_depth_image ? m_resolved_depth_image->getImage() : m_depth_image->getImage();
	}
	bool hasResolvedDepth() const { return m_resolved_depth_image != nullptr; }
	const std::vector<vk::Image>& getSwapChainImages() const { return m_swap_chain_images; }
	const std::vector<vk::raii::ImageView>& getSwapChainImageViews() const { return m_swap_chain_image_views; }
	float getExtentAspectRatio() const;
	vk::Extent2D getOffscreenExtent() const { return m_offscreen_extent; }

	void resizeOffscreenResources(vk::Extent2D extent);
	bool compareSwapFormats(const VeSwapChain& other) const;
	vk::Result acquireNextImage(uint32_t* imageIndex);
	void submitCompute(vk::CommandBuffer commandBuffer);

	// Scene-frame submits. depth_compute_follows=true means a shadow_graphics + depth_compute
	// pair will be submitted between pre_swap_graphics and swap_graphics, so swap_graphics
	// will wait on depth_compute's signal instead of pre_swap_graphics's.
	void prepareSubmitValues(bool depth_compute_follows);
	void submitPreSwapGraphics(vk::CommandBuffer cb);
	void submitShadowGraphics(vk::CommandBuffer cb);
	void submitDepthCompute(vk::CommandBuffer cb);
	vk::Result submitSwapGraphicsAndPresent(vk::CommandBuffer scene_cb, vk::CommandBuffer ui_cb, uint32_t* imageIndex);

	// Single-submit scene fallback 
	vk::Result submitSceneAndPresent(vk::CommandBuffer scene_cb, vk::CommandBuffer ui_cb, uint32_t* imageIndex);

	// Submit only a UI command buffer (no scene work; no timeline involvement).
	vk::Result submitUIOnly(vk::CommandBuffer ui_cb, uint32_t* imageIndex);

	void waitForCurrentFence();
	void resetCurrentFence();
	void advanceFrame();
	void beginTimelineFrame();
	void transitionImageLayout(
		vk::raii::CommandBuffer& command_buffer,
		uint32_t image_index,
		vk::ImageLayout old_layout,
		vk::ImageLayout new_layout,
		vk::AccessFlags2 src_access_mask,
		vk::AccessFlags2 dst_access_mask,
		vk::PipelineStageFlags2 src_stage,
		vk::PipelineStageFlags2 dst_stage
	);
	void transitionResolveTargetLayout(
		vk::raii::CommandBuffer& command_buffer,
		vk::ImageLayout old_layout,
		vk::ImageLayout new_layout,
		vk::AccessFlags2 src_access_mask,
		vk::AccessFlags2 dst_access_mask,
		vk::PipelineStageFlags2 src_stage,
		vk::PipelineStageFlags2 dst_stage
	);

private:
	void init();
	void createSwapChain();
	void createSwapChainImageViews();
	void createColorResources();
	void createDepthResources();
	void createSyncObjects();

	vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available_formats);
	vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& available_present_modes);
	vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities);

	// We declare the swapchain before image views so that views are destroyed first.
	VeDevice& m_ve_device;                 // (not owned) must outlive swapchain
	vk::Extent2D m_window_extent;

	vk::raii::SwapchainKHR m_swap_chain{nullptr};
	std::vector<vk::Image> m_swap_chain_images;
	std::vector<vk::raii::ImageView> m_swap_chain_image_views;

	//depth/color resources
	std::unique_ptr<VeImage> m_color_image;
	std::unique_ptr<VeImage> m_resolve_target_image;
	std::unique_ptr<VeImage> m_depth_image;
	std::unique_ptr<VeImage> m_resolved_depth_image;  // 1x resolve target when MSAA active
	vk::PresentModeKHR m_present_mode;
	vk::SampleCountFlagBits m_desired_num_samples;
	bool m_hdr_enabled;

	// Timeline semaphore shared by compute and graphics for async synchronization.
	vk::raii::Semaphore m_frame_timeline{nullptr};
	uint64_t m_frame_timeline_value = 0;
	uint64_t m_compute_signal_value = 0;
	uint64_t m_pre_swap_signal_value = 0;
	uint64_t m_depth_compute_signal_value = 0;   // set only when depth_compute_follows is true
	uint64_t m_swap_wait_value = 0;              // pre_swap (default) or depth_compute (split)
	std::vector<vk::raii::Fence> m_in_flight_fences;
	// Per-swapchain-image binary semaphores signaled by graphics submit and waited by present
	std::vector<vk::raii::Semaphore> m_render_finished_semaphores;
	// Per-frame binary semaphores signaled by acquire and waited by graphics submit
	std::vector<vk::raii::Semaphore> m_image_available_semaphores;

	SwapChainSupportDetails m_swap_chain_support;
	vk::SurfaceFormatKHR m_surface_format;
	vk::Extent2D m_swap_chain_extent;
	vk::Extent2D m_offscreen_extent;  // scene render resolution (may differ from swapchain in editor mode)
	vk::Format m_swap_chain_image_format;
	vk::Format m_offscreen_image_format;

	std::shared_ptr<VeSwapChain> m_old_swap_chain; // kept alive during recreation only

	uint32_t m_current_frame = 0;
};
}