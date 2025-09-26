#pragma once

#include "ve_device.hpp"
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

		uint32_t width() { return swap_chain_extent.width; }
		uint32_t height() { return swap_chain_extent.height; }
		size_t getImageCount() { return swap_chain_images.size(); }
		uint32_t getCurrentFrame() const { return current_frame; }
		vk::raii::SwapchainKHR* getSwapChain() { return &swap_chain; }
		vk::Format getSwapChainImageFormat() { return swap_chain_image_format; }
		vk::Extent2D getSwapChainExtent() { return swap_chain_extent; }
		vk::raii::ImageView* getImageView(size_t index) { return &swap_chain_image_views[index]; };
		const std::vector<vk::Image>& getSwapChainImages() { return swap_chain_images; }
		const std::vector<vk::raii::ImageView>& getSwapChainImageViews() { return swap_chain_image_views; }
		float extentAspectRatio() { return static_cast<float>(swap_chain_extent.width) /
								   static_cast<float>(swap_chain_extent.height); }

		vk::Result acquireNextImage(uint32_t* imageIndex);
		// Submits the provided command buffer, along with fence, and presents the acquired image.
		vk::Result submitAndPresent(vk::CommandBuffer commandBuffer, uint32_t* imageIndex);
		void waitForFences();
		void resetFences();
		void advanceFrame();

		bool compareSwapFormats(const VeSwapChain& other) const {
			return (other.swap_chain_image_format == swap_chain_image_format);
			// todo probably need to add more here in the future, for example depth format
		};

	private:
		void init();
		void createSwapChain();
		void createImageViews();
		void createSyncObjects();

		vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available_formats);
		vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& available_present_modes);
		vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities);

		// Order matters: C++ destroys members in reverse declaration order.
		// We declare the swapchain before image views so that views are destroyed first.
		VeDevice& ve_device;                 // (not owned) must outlive swapchain
		vk::Extent2D window_extent;

		vk::raii::SwapchainKHR swap_chain{nullptr};
		std::vector<vk::Image> swap_chain_images;
		std::vector<vk::raii::ImageView> swap_chain_image_views;

		// Synchronization primitives
		std::vector<vk::raii::Semaphore> present_complete_semaphores;
		std::vector<vk::raii::Semaphore> render_finished_semaphores;
		std::vector<vk::raii::Fence> in_flight_fences;

		SwapChainSupportDetails swap_chain_support;
		vk::SurfaceFormatKHR surface_format;
		vk::PresentModeKHR present_mode;
		vk::Extent2D swap_chain_extent;
		vk::Format swap_chain_image_format;

		std::shared_ptr<VeSwapChain> old_swap_chain; // kept alive during recreation only

		uint32_t current_frame = 0;
		uint32_t semaphore_index = 0;
	};
}