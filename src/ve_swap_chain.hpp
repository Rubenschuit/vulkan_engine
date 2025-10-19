/* VeSwapChain is responsible for managing the swap chain and
its associated resources. This includes image views, depth
resources and synchronization objects. */
#pragma once

#include "ve_device.hpp"
#include "ve_image.hpp"
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <ve_config.hpp>

#include <vector>
#include <string>
#include <memory>

namespace ve {

class VeSwapChain {
public:
	VeSwapChain(VeDevice& device, vk::Extent2D window_extent);
	VeSwapChain(VeDevice& device, vk::Extent2D window_extent, std::shared_ptr<VeSwapChain> old_swap_chain);
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
	vk::Extent2D getSwapChainExtent() const { return m_swap_chain_extent; }
	const vk::raii::ImageView& getImageView(size_t index) const { return m_swap_chain_image_views[index]; };
	const vk::raii::ImageView& getDepthImageView() const { return m_depth_image->getImageView(); }
	const vk::raii::Image& getDepthImage() const { return m_depth_image->getImage(); }
	const std::vector<vk::Image>& getSwapChainImages() const { return m_swap_chain_images; }
	const std::vector<vk::raii::ImageView>& getSwapChainImageViews() const { return m_swap_chain_image_views; }
	float getExtentAspectRatio() const;

	bool compareSwapFormats(const VeSwapChain& other) const;
	vk::Result acquireNextImage(uint32_t* imageIndex);
	// Todo use raii
	void submitComputeWork(vk::CommandBuffer commandBuffer);
	vk::Result submitAndPresent(vk::CommandBuffer commandBuffer, uint32_t* imageIndex);
	void waitForCurrentFence();
	void resetCurrentFence();
	void advanceFrame();
	void updateTimelineValues();
	void transitionImageLayout( vk::raii::CommandBuffer& command_buffer,
								uint32_t image_index,
								vk::ImageLayout old_layout,
								vk::ImageLayout new_layout,
								vk::AccessFlags2 src_access_mask,
								vk::AccessFlags2 dst_access_mask,
								vk::PipelineStageFlags2 src_stage,
								vk::PipelineStageFlags2 dst_stage);

private:
	void init();
	void createSwapChain();
	void createSwapChainImageViews();
	void createDepthResources();
	void createSyncObjects();

	vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available_formats);
	vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& available_present_modes);
	vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities);

	// Order matters: C++ destroys members in reverse declaration order.
	// We declare the swapchain before image views so that views are destroyed first.
	VeDevice& m_ve_device;                 // (not owned) must outlive swapchain
	vk::Extent2D m_window_extent;

	vk::raii::SwapchainKHR m_swap_chain{nullptr};
	std::vector<vk::Image> m_swap_chain_images;
	std::vector<vk::raii::ImageView> m_swap_chain_image_views;

	//depth resources
	std::unique_ptr<VeImage> m_depth_image;

	// Synchronization primitives
	// TODO: consider moving the timeline semaphore somewhere else
	vk::raii::Semaphore semaphore{nullptr};
	uint64_t timeline_value = 0;
	uint64_t compute_wait_value;
	uint64_t compute_signal_value;
	uint64_t graphics_wait_value;
	uint64_t graphics_signal_value;
	std::vector<vk::raii::Fence> m_in_flight_fences;
	// Per-swapchain-image binary semaphores signaled by graphics submit and waited by present
	std::vector<vk::raii::Semaphore> m_render_finished_semaphores;
	// Per-frame binary semaphores signaled by acquire and waited by graphics submit
	std::vector<vk::raii::Semaphore> m_image_available_semaphores;

	SwapChainSupportDetails m_swap_chain_support;
	vk::SurfaceFormatKHR m_surface_format;
	vk::PresentModeKHR m_present_mode;
	vk::Extent2D m_swap_chain_extent;
	vk::Format m_swap_chain_image_format;

	std::shared_ptr<VeSwapChain> m_old_swap_chain; // kept alive during recreation only

	uint32_t m_current_frame = 0;
};
}