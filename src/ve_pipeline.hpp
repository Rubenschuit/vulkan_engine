# pragma once

#include "ve_device.hpp"

#include <string>
#include <vector>

namespace ve {
    struct PipelineConfigInfo {
        std::vector<vk::DynamicState> dynamicStateEnables;
        vk::PipelineDynamicStateCreateInfo dynamicStateInfo{};

        vk::Viewport viewport;
        vk::Rect2D scissor;
        vk::PipelineInputAssemblyStateCreateInfo inputAssemblyInfo;
        vk::PipelineRasterizationStateCreateInfo rasterizationInfo;
        vk::PipelineMultisampleStateCreateInfo multisampleInfo;
        vk::PipelineColorBlendAttachmentState colorBlendAttachment;
        vk::PipelineColorBlendStateCreateInfo colorBlendInfo;
        vk::PipelineDepthStencilStateCreateInfo depthStencilInfo;
        vk::PipelineLayout pipelineLayout = nullptr;
        vk::RenderPass renderPass = nullptr;
        uint32_t subpass = 0;
        vk::Format colorFormat = vk::Format::eUndefined;
        vk::Format depthFormat = vk::Format::eUndefined;

        //PipelineConfigInfo() = default;
        //PipelineConfigInfo(const PipelineConfigInfo&) = delete;
        //PipelineConfigInfo& operator=(const PipelineConfigInfo&) = delete;

        //std::vector<vk::VertexInputBindingDescription> bindingDescriptions{};
        //std::vector<vk::VertexInputAttributeDescription> attributeDescriptions{};
        //std::vector<vk::DynamicState> dynamicStateEnables{};
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
        static PipelineConfigInfo defaultPipelineConfigInfo(uint32_t width, uint32_t height);

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