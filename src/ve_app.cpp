#include "ve_app.hpp"

namespace ve {
    VeApp::VeApp() {
        createPipelineLayout();
        createPipeline();
        createCommandBuffers();
    }

    VeApp::~VeApp() {
    }

    void VeApp::run() {
        mainLoop();
        cleanup();
    }

    void VeApp::mainLoop() {
        while (!glfwWindowShouldClose(window.getGLFWwindow())) {
            glfwPollEvents();
            drawFrame();
        }
        device.getDevice().waitIdle();
    }

    void VeApp::cleanup() {
        // nothing yet
    }

    void VeApp::createPipelineLayout() {
        vk::PipelineLayoutCreateInfo pipeline_layout_info{
            .sType = vk::StructureType::ePipelineLayoutCreateInfo,
            .setLayoutCount = 0,
            .pSetLayouts = nullptr,
            .pushConstantRangeCount = 0,
            .pPushConstantRanges = nullptr
        };
        pipeline_layout = vk::raii::PipelineLayout(device.getDevice(), pipeline_layout_info);
    }

    void VeApp::createPipeline() {
        auto pipeline_config = VePipeline::defaultPipelineConfigInfo();
        // set formats for dynamic rendering
        pipeline_config.color_format = swap_chain->getSwapChainImageFormat();
        pipeline_config.pipeline_layout = pipeline_layout;
        pipeline = std::make_unique<VePipeline>(
            device, 
            "../shaders/simple_shader.vert.spv", 
            "../shaders/simple_shader.frag.spv", 
            pipeline_config);
    }

    void VeApp::createCommandBuffers() {
        // Allocate a single command buffer for now
        vk::CommandBufferAllocateInfo alloc_info{
            .sType = vk::StructureType::eCommandBufferAllocateInfo,
            .commandPool = *device.getCommandPool(),
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = ve::MAX_FRAMES_IN_FLIGHT
        };
        command_buffers = vk::raii::CommandBuffers(device.getDevice(), alloc_info);
    }

    void VeApp::recordCommandBuffer(uint32_t imageIndex) {
        auto extent = swap_chain->getSwapChainExtent();
        auto height = extent.height;
        auto width = extent.width;
        uint32_t current_frame = swap_chain->getCurrentFrame();
        
        command_buffers[current_frame].begin( {} );
        // Before starting rendering, transition the swapchain image to COLOR_ATTACHMENT_OPTIMAL
        transitionImageLayout(
            imageIndex,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal,
            {},                                                     
            vk::AccessFlagBits2::eColorAttachmentWrite,               
            vk::PipelineStageFlagBits2::eTopOfPipe,                  
            vk::PipelineStageFlagBits2::eColorAttachmentOutput       
        );
        vk::ClearValue clearColor = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f);
        vk::RenderingAttachmentInfo attachmentInfo = {
            .imageView = swap_chain->getSwapChainImageViews()[imageIndex],
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = clearColor
        };
        vk::RenderingInfo renderingInfo = {
            .renderArea = { .offset = { 0, 0 }, .extent = extent },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachmentInfo
        };

        command_buffers[current_frame].beginRendering(renderingInfo);
        command_buffers[current_frame].bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());
        command_buffers[current_frame].setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f));
        command_buffers[current_frame].setScissor( 0, vk::Rect2D( vk::Offset2D( 0, 0 ), extent ) );
        command_buffers[current_frame].draw(3, 1, 0, 0);
        command_buffers[current_frame].endRendering();
        // After rendering, transition to presentation
        transitionImageLayout(
            imageIndex,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::ePresentSrcKHR,
            vk::AccessFlagBits2::eColorAttachmentWrite,                
            {},                                                      
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,      
            vk::PipelineStageFlagBits2::eBottomOfPipe                  
        );
        command_buffers[current_frame].end();
    }

    void VeApp::transitionImageLayout(
        uint32_t image_index,
        vk::ImageLayout old_layout,
        vk::ImageLayout new_layout,
        vk::AccessFlags2 src_access_mask,
        vk::AccessFlags2 dst_access_mask,
        vk::PipelineStageFlags2 src_stage_mask,
        vk::PipelineStageFlags2 dst_stage_mask
        ) {
        vk::ImageMemoryBarrier2 barrier = {
            .srcStageMask = src_stage_mask,
            .srcAccessMask = src_access_mask,
            .dstStageMask = dst_stage_mask,
            .dstAccessMask = dst_access_mask,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swap_chain->getSwapChainImages()[image_index],
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        vk::DependencyInfo dependency_info = {
            .dependencyFlags = {},
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier
        };
        uint32_t current_frame = swap_chain->getCurrentFrame();
        command_buffers[current_frame].pipelineBarrier2(dependency_info);
    }

    void VeApp::recreateSwapChain() {
        // Handle minimized window
        int width = 0, height = 0;
        glfwGetFramebufferSize(window.getGLFWwindow(), &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window.getGLFWwindow(), &width, &height);
            glfwWaitEvents();
        }

        device.getDevice().waitIdle();

        // Create a new swap chain
        if (swap_chain == nullptr) {
            swap_chain = std::make_unique<VeSwapChain>(device, window.getExtent());
        } 
        else {
            // Transfer ownership of the existing swap chain to a shared_ptr so the new one
            // can safely reference it during recreation.
            std::shared_ptr<VeSwapChain> old_swap_chain{ std::move(swap_chain) };
            swap_chain = std::make_unique<VeSwapChain>(device, window.getExtent(), old_swap_chain);
            if (!old_swap_chain->compareSwapFormats(*swap_chain)) {
                throw std::runtime_error("Swap chain image (or depth) format has changed!");
            }
        }
    }
        
    void VeApp::drawFrame() {
        // Wait until the previous frame is finished
        swap_chain->waitForFences();

        // Acquire an image from the swap chain
        uint32_t image_index;
        auto result = swap_chain->acquireNextImage(&image_index);
        if (result == vk::Result::eErrorOutOfDateKHR) {
            recreateSwapChain();
            return;
        }
        if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
            throw std::runtime_error("failed to acquire swap chain image!");
        }

        // Reset the fence for the current frame
        swap_chain->resetFences();

        // Record command buffer using the acquired image
        uint32_t current_frame = swap_chain->getCurrentFrame();
        command_buffers[current_frame].reset();
        recordCommandBuffer(image_index);

        // Submit the command buffer
        result = swap_chain->submitAndPresent(*command_buffers[current_frame], &image_index);
        if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR || window.framebuffer_resized) {
            window.framebuffer_resized = false;
            recreateSwapChain();
        } else if (result != vk::Result::eSuccess) {
            throw std::runtime_error("failed to present swap chain image!");
        }
        // Advance to the next frame
        swap_chain->advanceFrame();
    }


}
        
