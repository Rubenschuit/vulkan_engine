/* VeSwapChain is owned by VeRenderer and is responsible for managing the swap chain
 and its associated resources. This includes image views, depth resources and
synchronization objects. Also sets the number of samples for MSAA. */
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
	const vk::raii::Image& getDepthImage() const { return m_depth_image->getImage(); }
	/// Single-sample depth (resolved from MSAA prepass, or same as depth when no MSAA)
	const vk::raii::ImageView& getResolvedDepthImageView() const {
		return m_resolved_depth_image ? m_resolved_depth_image->getImageView() : m_depth_image->getImageView();
	}
	const vk::raii::Image& getResolvedDepthImage() const {
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
	void submitComputeWork(vk::CommandBuffer commandBuffer);
	vk::Result submitAndPresent(vk::CommandBuffer scene_cb, vk::CommandBuffer ui_cb, uint32_t* imageIndex);
	void waitForCurrentFence();
	void resetCurrentFence();
	void advanceFrame();
	void updateTimelineValues();
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

	// Timeline semaphore: only compute signals, graphics waits.
	vk::raii::Semaphore m_compute_timeline{nullptr};
	uint64_t m_compute_timeline_value = 0;
	uint64_t m_compute_signal_value;
	uint64_t m_graphics_wait_value;
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