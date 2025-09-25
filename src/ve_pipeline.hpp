# pragma once

#include "ve_device.hpp"

#include <string>
#include <vector>

namespace ve {
    struct PipelineConfigInfo {
        std::vector<vk::DynamicState> dynamic_state_enables{};
        vk::PipelineDynamicStateCreateInfo dynamic_state_info{};
        vk::PipelineInputAssemblyStateCreateInfo input_assembly_info{} ;
        vk::PipelineRasterizationStateCreateInfo rasterization_info{};
        vk::PipelineMultisampleStateCreateInfo multisample_info{};
        vk::PipelineViewportStateCreateInfo viewport_info{};
        vk::PipelineColorBlendAttachmentState color_blend_attachment{} ;
        vk::PipelineColorBlendStateCreateInfo color_blend_info{};
        vk::PipelineLayout pipeline_layout = nullptr;
        vk::RenderPass render_pass = nullptr;
        uint32_t subpass = 0;
        vk::Format color_format = vk::Format::eUndefined;
        //vk::Format depth_format = vk::Format::eUndefined;
    };

    class VePipeline {
    public:
        VePipeline(
            VeDevice& device,
            const std::string& vert_file_path,
            const std::string& frag_file_path,
            const PipelineConfigInfo& config_info);

        ~VePipeline();

        // Prevent copying
        VePipeline(const VePipeline&) = delete;
        void operator=(const VePipeline&) = delete;

        vk::Pipeline getPipeline() { return *graphics_pipeline; }
        static PipelineConfigInfo defaultPipelineConfigInfo();

    private:
        static std::vector<char> readFile(const std::string& file_path);

        void createGraphicsPipeline(
            const std::string& vert_file_path, 
            const std::string& frag_file_path,
            const PipelineConfigInfo& config_info);

        void createShaderModule(const std::vector<char>& code, vk::raii::ShaderModule* shader_module);

        VeDevice& ve_device; // will outlive the pipeline class
        vk::raii::Pipeline graphics_pipeline{nullptr};
        vk::raii::ShaderModule vert_shader_module{nullptr};
        vk::raii::ShaderModule frag_shader_module{nullptr};

    };
}