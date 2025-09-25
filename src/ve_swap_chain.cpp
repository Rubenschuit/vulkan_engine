#include "ve_swap_chain.hpp"

#include <stdexcept>
#include <array>
#include <iostream>
#include <set>
#include <limits>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cassert>

namespace ve {

    VeSwapChain::VeSwapChain(VeDevice& device, vk::Extent2D window_extent) 
        : device(device), window_extent(window_extent) {
        init();
    }
    
    VeSwapChain::VeSwapChain(VeDevice& device, vk::Extent2D window_extent, std::shared_ptr<VeSwapChain> old_swap_chain) 
        : device(device), window_extent(window_extent), old_swap_chain(old_swap_chain) {
        init();
        old_swap_chain = nullptr;
    }

    VeSwapChain::~VeSwapChain() {
        // Cleanup non-RAII objects
        for(size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(*device.getDevice(), image_available_semaphores[i], nullptr);
            vkDestroySemaphore(*device.getDevice(), render_finished_semaphores[i], nullptr);
            vkDestroyFence(*device.getDevice(), in_flight_fences[i], nullptr);
        }
    }

    void VeSwapChain::init() {
        createSwapChain();
        createImageViews();
        createSyncObjects();
    }

    // Todo: 
    vk::Result VeSwapChain::acquireNextImage(uint32_t* image_ndex) {

        vkWaitForFences(*device.getDevice(), 1, 
                        reinterpret_cast<const VkFence *>(&in_flight_fences[current_frame]), 
                        VK_TRUE, 
                        std::numeric_limits<uint64_t>::max());

        auto [result, image_index] = swap_chain.acquireNextImage(
            std::numeric_limits<uint64_t>::max(), 
            image_available_semaphores[current_frame], 
            nullptr);

        //device.getDevice().resetFences(1, &in_flight_fences[current_frame]);
        return result;
    }

    // This function first waits on the fence for the current frame to ensure that the previous frame has finished.
    // It then checks if the image to be rendered to is already in use by another frame, and if so, waits on the corresponding fence.
    // After that, it sets up the semaphores and submits the command buffer to the graphics queue.
    // Finally, it presents the image and advances to the next frame.
    vk::Result VeSwapChain::submitCommandBuffers(const vk::CommandBuffer* buffers, uint32_t* imageIndex) {
        if (images_in_flight[*imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(*device.getDevice(), 1, 
                            reinterpret_cast<const VkFence *>(&images_in_flight[*imageIndex]), 
                            VK_TRUE, 
                            std::numeric_limits<uint64_t>::max());
        }
        images_in_flight[*imageIndex] = in_flight_fences[current_frame];

        vk::Semaphore wait_semaphores[] = {image_available_semaphores[current_frame]};
        vk::PipelineStageFlags wait_stages[] = {vk::PipelineStageFlagBits::eColorAttachmentOutput};
        vk::Semaphore signal_semaphores[] = {render_finished_semaphores[current_frame]};

        vk::SubmitInfo submitInfo{
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = wait_semaphores,
            .pWaitDstStageMask = wait_stages,
            .commandBufferCount = 1,
            .pCommandBuffers = buffers,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = signal_semaphores
        };

        vkResetFences(*device.getDevice(), 1, &in_flight_fences[current_frame]); 

        device.getQueue().submit(submitInfo, in_flight_fences[current_frame]);

        vk::SwapchainKHR swapChains[] = { *swap_chain };

        vk::PresentInfoKHR presentInfo{
            .sType = vk::StructureType::ePresentInfoKHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = signal_semaphores,
            .swapchainCount = 1,
            .pSwapchains = swapChains,
            .pImageIndices = imageIndex
        };

        auto result = device.getQueue().presentKHR(presentInfo);

        current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;

        return result;
    }

    void VeSwapChain::createSwapChain() {
        swap_chain_support = device.getSwapChainSupport();
        surface_format = chooseSwapSurfaceFormat(swap_chain_support.formats);
        present_mode = chooseSwapPresentMode(swap_chain_support.presentModes);
        swap_chain_extent = chooseSwapExtent(swap_chain_support.capabilities);
        swap_chain_image_format = surface_format.format;

        // try to use one more than the minimum number of images to improve gpu utilization
        // note that there is no guarantee that we can get that many images, so we need to check against the maximum as well
        uint32_t image_count = swap_chain_support.capabilities.minImageCount + 1;
        if (swap_chain_support.capabilities.maxImageCount > 0 && image_count > swap_chain_support.capabilities.maxImageCount) {
            image_count = swap_chain_support.capabilities.maxImageCount;
        }

        vk::SwapchainCreateInfoKHR create_info{
            .sType = vk::StructureType::eSwapchainCreateInfoKHR,
            .surface = *device.getSurface(),
            .minImageCount = image_count,
            .imageFormat = surface_format.format,
            .imageColorSpace = surface_format.colorSpace,
            .imageExtent = swap_chain_extent,
            .imageArrayLayers = 1, // always 1 unless developing stereoscopic 3D app
            .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
            .preTransform = swap_chain_support.capabilities.currentTransform,
            .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
            .presentMode = present_mode,
            .clipped = VK_TRUE,
            .oldSwapchain = VK_NULL_HANDLE, // no swap chain swapping yet
            
            // graphics and presentation are in the same queue family, so we can use exclusive mode
            .imageSharingMode = vk::SharingMode::eExclusive, // best performance
            .queueFamilyIndexCount = 0, // optional
            .pQueueFamilyIndices = nullptr // optional
        };

        swap_chain = vk::raii::SwapchainKHR(device.getDevice(), create_info);
        swap_chain_images = swap_chain.getImages();
    }

    void VeSwapChain::createImageViews() {

        //swap_chain_image_views.resize(swap_chain_images.size());

        assert(swap_chain_image_views.empty());
        vk::ImageViewCreateInfo imageViewCreateInfo{ 
            .viewType = vk::ImageViewType::e2D, 
            .format = surface_format.format,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 } 
        };
        for ( auto image : swap_chain_images )
        {
            imageViewCreateInfo.image = image;
            swap_chain_image_views.emplace_back( device.getDevice(), imageViewCreateInfo );
        }
    }

    void VeSwapChain::createSyncObjects() {
        image_available_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
        render_finished_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
        in_flight_fences.resize(MAX_FRAMES_IN_FLIGHT);
        images_in_flight.resize(swap_chain_images.size(), VK_NULL_HANDLE);

        vk::SemaphoreCreateInfo semaphore_info{
            .sType = vk::StructureType::eSemaphoreCreateInfo
        };

        vk::FenceCreateInfo fence_info{
            .sType = vk::StructureType::eFenceCreateInfo,
            .flags = vk::FenceCreateFlagBits::eSignaled
        };

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(*device.getDevice(), reinterpret_cast<const VkSemaphoreCreateInfo*>(&semaphore_info), nullptr, &image_available_semaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(*device.getDevice(), reinterpret_cast<const VkSemaphoreCreateInfo*>(&semaphore_info), nullptr, &render_finished_semaphores[i]) != VK_SUCCESS ||
                vkCreateFence(*device.getDevice(), reinterpret_cast<const VkFenceCreateInfo*>(&fence_info), nullptr, &in_flight_fences[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create synchronization objects for a frame!");
            }
        }
    }

    // Choose the surface format, which is a combination of color depth and color space.
    // Prefer 8-bit BGRA and SRGB nonlinear color space for now.
    vk::SurfaceFormatKHR VeSwapChain::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available_formats) {
        for (const auto& available_format : available_formats) {
            if (available_format.format == vk::Format::eB8G8R8A8Srgb && 
                available_format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                return available_format;
            }
        }
        return available_formats[0];
    }

    // Choose the presentation mode, which is how images are presented to the screen.
    // Mailbox is the lowest latency non-tearing mode, but may not be available.
    // Fifo is always available, but may have higher latency.
    // Immediate may be available, and is lowest latency, but may have tearing.
    vk::PresentModeKHR VeSwapChain::chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& available_present_modes) {
        for (const auto& available_present_mode : available_present_modes) {
            if (available_present_mode == vk::PresentModeKHR::eMailbox) {
                return available_present_mode;
            }
        }
        return vk::PresentModeKHR::eFifo;
    }

    // Choose the resolution (pixels) of the swap chain images.
    // If the surface size is defined, the swap chain size must match.
    // If the surface size is undefined, the swap chain size can be set to
    // the size of the window, but must be within the min and max bounds.
    vk::Extent2D VeSwapChain::chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        } else {
            vk::Extent2D actual_extent = window_extent;
            actual_extent.width = std::clamp(actual_extent.width, 
                                             capabilities.minImageExtent.width, 
                                             capabilities.maxImageExtent.width);
            actual_extent.height = std::clamp(actual_extent.height, 
                                              capabilities.minImageExtent.height, 
                                              capabilities.maxImageExtent.height);
            return actual_extent;
        }
    }
    
}

