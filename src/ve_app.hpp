#pragma once
#include <ve_window.hpp>
#include <ve_device.hpp>
#include <ve_pipeline.hpp>
#include <ve_swap_chain.hpp>

#include <memory>
#include <vector>
#include <iostream>

namespace ve {
    class VeApp {
    public:
        VeApp();
        ~VeApp();

        //destroy copy and move constructors and assignment operators
        VeApp(const VeApp&) = delete;
        VeApp& operator=(const VeApp&) = delete;

        static constexpr int WIDTH = 800;
        static constexpr int HEIGHT = 600;

        void run();

    private:
        void createPipeline();
        void createPipelineLayout();
        void createCommandBuffers();
        void recordCommandBuffer(uint32_t imageIndex);
        void transition_image_layout(
            uint32_t imageIndex,
            vk::ImageLayout oldLayout,
            vk::ImageLayout newLayout,
            vk::AccessFlags2 srcAccessMask,
            vk::AccessFlags2 dstAccessMask,
            vk::PipelineStageFlags2 srcStage,
            vk::PipelineStageFlags2 dstStage);
        void drawFrame();

        VeWindow window{WIDTH, HEIGHT, "Vulkan Engine!"};
        VeDevice device{window};
        VeSwapChain swap_chain{device, window.getExtent()};
        vk::raii::PipelineLayout pipeline_layout{nullptr};
        std::unique_ptr<VePipeline> pipeline;
        vk::raii::CommandBuffer command_buffer{nullptr};
    };
}

