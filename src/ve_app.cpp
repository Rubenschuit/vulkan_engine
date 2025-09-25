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
        while (!glfwWindowShouldClose(window.getGLFWwindow())) {
            glfwPollEvents();
            drawFrame();
        }

        device.getDevice().waitIdle();
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
        auto pipeline_config = VePipeline::defaultPipelineConfigInfo(swap_chain.width(), swap_chain.height());
        // set formats for dynamic rendering
        pipeline_config.colorFormat = swap_chain.getSwapChainImageFormat();
        pipeline_config.pipelineLayout = pipeline_layout;
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
            .commandBufferCount = 1
        };
        command_buffer = std::move(vk::raii::CommandBuffers(device.getDevice(), alloc_info).front());
    }

    void VeApp::recordCommandBuffer(uint32_t imageIndex) {
        auto extent = swap_chain.getSwapChainExtent();
        auto height = extent.height;
        auto width = extent.width;
        
        command_buffer.begin( {} );
        // Before starting rendering, transition the swapchain image to COLOR_ATTACHMENT_OPTIMAL
        transition_image_layout(
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
            .imageView = swap_chain.getSwapChainImageViews()[imageIndex],
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

        command_buffer.beginRendering(renderingInfo);
        command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());
        command_buffer.setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f));
        command_buffer.setScissor( 0, vk::Rect2D( vk::Offset2D( 0, 0 ), extent ) );
        command_buffer.draw(3, 1, 0, 0);
        command_buffer.endRendering();
        // After rendering, transition to presentation
        transition_image_layout(
            imageIndex,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::ePresentSrcKHR,
            vk::AccessFlagBits2::eColorAttachmentWrite,                
            {},                                                      
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,      
            vk::PipelineStageFlagBits2::eBottomOfPipe                  
        );
        command_buffer.end();
    }

    void VeApp::transition_image_layout(
        uint32_t currentFrame,
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
            .image = swap_chain.getSwapChainImages()[currentFrame],
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
        command_buffer.pipelineBarrier2(dependency_info);
    }
        
    void VeApp::drawFrame() {
        uint32_t image_index;
        auto result = swap_chain.acquireNextImage(&image_index);
        if (result == vk::Result::eErrorOutOfDateKHR) {
            return;
        }
        else if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
            throw std::runtime_error("failed to acquire swap chain image!");
        }
        //command_buffer.reset();
        recordCommandBuffer(image_index);
        result = swap_chain.submitCommandBuffers(&*command_buffer, &image_index);
        switch ( result )
        {
            case vk::Result::eSuccess: break;
            case vk::Result::eSuboptimalKHR: std::cout << "vk::Queue::presentKHR returned vk::Result::eSuboptimalKHR !\n"; break;
            default: break;  // an unexpected result is returned!
        }
    }

}
        
