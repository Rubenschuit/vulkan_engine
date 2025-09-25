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

        uint32_t getCurrentFrame() const { return current_frame; }
        vk::raii::ImageView *getImageView(size_t index) { return &swap_chain_image_views[index]; };
        size_t getImageCount() { return swap_chain_images.size(); }
        vk::Format getSwapChainImageFormat() { return swap_chain_image_format; }
        const std::vector<vk::Image>& getSwapChainImages() { return swap_chain_images; }
        const std::vector<vk::raii::ImageView>& getSwapChainImageViews() { return swap_chain_image_views; }
        vk::Extent2D getSwapChainExtent() { return swap_chain_extent; }
        uint32_t width() { return swap_chain_extent.width; }
        uint32_t height() { return swap_chain_extent.height; }
        float extentAspectRatio() { return static_cast<float>(swap_chain_extent.width) / 
                                           static_cast<float>(swap_chain_extent.height);  }

        vk::Result acquireNextImage(uint32_t* imageIndex);
        vk::Result submitCommandBuffer(vk::CommandBuffer commandBuffer, uint32_t* imageIndex);
        bool compareSwapFormats(const VeSwapChain& other) const;

    private:
        void init();
        void createSwapChain();
        void createImageViews();
        void createSyncObjects();

        vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available_formats);
        vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& available_present_modes);
        vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities);

        SwapChainSupportDetails swap_chain_support;
        vk::SurfaceFormatKHR surface_format;
        vk::PresentModeKHR present_mode;
        vk::Extent2D swap_chain_extent;

        vk::Format swap_chain_image_format;

        std::vector<vk::Image> swap_chain_images;
        std::vector<vk::raii::ImageView> swap_chain_image_views;

        VeDevice& device; 
        vk::Extent2D window_extent;

        vk::raii::SwapchainKHR swap_chain{nullptr};
        std::shared_ptr<VeSwapChain> old_swap_chain;

        std::vector<vk::raii::Semaphore> present_complete_semaphores;
        std::vector<vk::raii::Semaphore> render_finished_semaphores;
        std::vector<vk::raii::Fence> in_flight_fences;
        uint32_t current_frame = 0;
        uint32_t semaphore_index = 0;
    };
}