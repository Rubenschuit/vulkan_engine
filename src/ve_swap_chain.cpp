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

    VeSwapChain::~VeSwapChain() {}

    void VeSwapChain::init() {
        createSwapChain();
        createImageViews();
        createSyncObjects();
    }

    // Todo: 
    vk::Result VeSwapChain::acquireNextImage(uint32_t* image_index) {

        // Wait for the fence for the current frame to ensure that the previous frame has finished
        while ( vk::Result::eTimeout == 
                device.getDevice().waitForFences( *in_flight_fences[current_frame], vk::True, UINT64_MAX ) );

        // Acquire the next image from the swap chain allowing the semaphore to signal once the image is available
        auto [result, _image_index] = swap_chain.acquireNextImage(
            UINT64_MAX, 
            present_complete_semaphores[semaphore_index], 
            nullptr);
        *image_index = _image_index;
        // Reset the fence for the current frame
        device.getDevice().resetFences( *in_flight_fences[current_frame] );
        return result;
    }

    vk::Result VeSwapChain::submitCommandBuffer(vk::CommandBuffer commandBuffer, uint32_t* image_index) {
        vk::PipelineStageFlags wait_dest_stage_mask(vk::PipelineStageFlagBits::eColorAttachmentOutput);
        const vk::SubmitInfo submit_info{ 
            .waitSemaphoreCount = 1, 
            .pWaitSemaphores = &*present_complete_semaphores[semaphore_index],
            .pWaitDstStageMask = &wait_dest_stage_mask, 
            .commandBufferCount = 1, 
            .pCommandBuffers = &commandBuffer,
            .signalSemaphoreCount = 1, 
            .pSignalSemaphores = &*render_finished_semaphores[*image_index] };

        device.getQueue().submit(submit_info, *in_flight_fences[current_frame]);

        const vk::PresentInfoKHR present_info{ 
            .waitSemaphoreCount = 1, 
            .pWaitSemaphores = &*render_finished_semaphores[*image_index],
            .swapchainCount = 1, 
            .pSwapchains = &*swap_chain, 
            .pImageIndices = image_index };

        auto result = device.getQueue().presentKHR(present_info);

    current_frame = (current_frame + 1) % ve::MAX_FRAMES_IN_FLIGHT;
        semaphore_index = (semaphore_index + 1) % present_complete_semaphores.size();

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
        present_complete_semaphores.clear();
        render_finished_semaphores.clear();
        in_flight_fences.clear();

        vk::SemaphoreCreateInfo present_info {};
        vk::SemaphoreCreateInfo render_info {};
        vk::FenceCreateInfo fence_info { .flags = vk::FenceCreateFlagBits::eSignaled };

        for (size_t i = 0; i < swap_chain_images.size(); i++) {
            present_complete_semaphores.emplace_back(device.getDevice(), present_info);
            render_finished_semaphores.emplace_back(device.getDevice(), render_info);
        }

        for (size_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; i++) {
            in_flight_fences.emplace_back(device.getDevice(), fence_info);
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

