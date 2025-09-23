# pragma once

#include "ve_device.hpp"

#include <string>
#include <vector>

namespace ve {
    struct PipelineConfigInfo {};

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

        static PipelineConfigInfo defaultPipelineConfigInfo(uint32_t width, uint32_t height);

    private:
        static std::vector<char> readFile(const std::string& file_path);

        void createGraphicsPipeline(
            const std::string& vert_file_path, 
            const std::string& frag_file_path,
            const PipelineConfigInfo& config_info);

        void createShaderModule(const std::vector<char>& code, vk::raii::ShaderModule* shader_module);

        VeDevice& ve_device; // will outlive the containing class
        vk::raii::Pipeline graphics_pipeline{nullptr};
        vk::raii::ShaderModule vert_shader_module{nullptr};
        vk::raii::ShaderModule frag_shader_module{nullptr};

    };
}