#pragma once

#include "ve_device.hpp"
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

#include <vector>
#include <string>
#include <memory>

namespace ve {
    class VeSwapChain {
    public:
        static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

        VeSwapChain(VeDevice& device, vk::Extent2D window_extent);
        VeSwapChain(VeDevice& device, vk::Extent2D window_extent, std::shared_ptr<VeSwapChain> old_swap_chain);
        ~VeSwapChain();

        // Not copyable or movable
        VeSwapChain(const VeSwapChain&) = delete;
        VeSwapChain& operator=(const VeSwapChain&) = delete;

        vk::Framebuffer getFrameBuffer(int index) { return swap_chain_framebuffers[index]; };
        vk::raii::RenderPass *getRenderPass() { return &render_pass; };
        vk::raii::ImageView *getImageView(int index) { return &swap_chain_image_views[index]; };
        size_t getImageCount() { return swap_chain_images.size(); }
        vk::Format getSwapChainImageFormat() { return swap_chain_image_format; }
        vk::Format getSwapChainDepthFormat() { return swap_chain_depth_format; }
        const std::vector<vk::Image>& getSwapChainImages() { return swap_chain_images; }
        const std::vector<vk::raii::ImageView>& getSwapChainImageViews() { return swap_chain_image_views; }
        vk::Extent2D getSwapChainExtent() { return swap_chain_extent; }
        uint32_t width() { return swap_chain_extent.width; }
        uint32_t height() { return swap_chain_extent.height; }
        float extentAspectRatio() { return static_cast<float>(swap_chain_extent.width) / 
                                           static_cast<float>(swap_chain_extent.height);  }

        vk::Format findDepthFormat();
        vk::Result acquireNextImage(uint32_t* imageIndex);
        vk::Result submitCommandBuffers(const vk::CommandBuffer* buffers, uint32_t* imageIndex);
        bool compareSwapFormats(const VeSwapChain& other) const;

    private:
        void init();
        void createSwapChain();
        void createImageViews();
        void createDepthResources();
        void createRenderPass();
        void createFramebuffers();
        void createSyncObjects();

        vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available_formats);
        vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& available_present_modes);
        vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities);

        SwapChainSupportDetails swap_chain_support;
        vk::SurfaceFormatKHR surface_format;
        vk::PresentModeKHR present_mode;
        vk::Extent2D swap_chain_extent;

        vk::Format swap_chain_image_format;
        vk::Format swap_chain_depth_format;

        std::vector<vk::raii::Framebuffer> swap_chain_framebuffers;
        vk::raii::RenderPass render_pass{nullptr};

        std::vector<vk::raii::Image> depth_images;
        std::vector<vk::raii::DeviceMemory> depth_image_memorys;
        std::vector<vk::raii::ImageView> depth_image_views;
        std::vector<vk::Image> swap_chain_images;
        std::vector<vk::raii::ImageView> swap_chain_image_views;

        VeDevice& device; 
        vk::Extent2D window_extent;

        vk::raii::SwapchainKHR swap_chain{nullptr};
        std::shared_ptr<VeSwapChain> old_swap_chain;

        //non raii for now
        std::vector<VkSemaphore> image_available_semaphores;
        std::vector<VkSemaphore> render_finished_semaphores;
        std::vector<VkFence> in_flight_fences;
        std::vector<VkFence> images_in_flight;
        size_t current_frame = 0;
    };
}