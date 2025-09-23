#include "ve_pipeline.hpp"

#include <fstream>
#include <stdexcept>
#include <iostream>

namespace ve {
    VePipeline::VePipeline(
            VeDevice& device, 
            const std::string& vert_file_path,
            const std::string& frag_file_path,
            const PipelineConfigInfo& config_info) : ve_device(device) {
        createGraphicsPipeline(vert_file_path, frag_file_path, config_info);
    }

    VePipeline::~VePipeline() {
        // Cleanup code for the pipeline would go here
    }

    PipelineConfigInfo VePipeline::defaultPipelineConfigInfo(uint32_t width, uint32_t height) {
        PipelineConfigInfo config_info{};
        // Set default values for the config_info as needed
        return config_info;
    }

    std::vector<char> VePipeline::readFile(const std::string& file_path) {
        std::ifstream file(file_path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("failed to open file: " + file_path);
        }

        size_t file_size = static_cast<size_t>(file.tellg());
        std::vector<char> buffer(file_size);

        file.seekg(0);
        file.read(buffer.data(), file_size);
        file.close();
        return buffer;
    }

    void VePipeline::createGraphicsPipeline(
            const std::string& vert_file_path, 
            const std::string& frag_file_path, 
            const PipelineConfigInfo& config_info) {

        auto vert_shader_code = readFile(vert_file_path);
        auto frag_shader_code = readFile(frag_file_path);

        std::cout << "Vertex shader code size: " << vert_shader_code.size() << " bytes" << std::endl;
        std::cout << "Fragment shader code size: " << frag_shader_code.size() << " bytes" << std::endl;
    }

    void VePipeline::createShaderModule(const std::vector<char>& code, vk::raii::ShaderModule* shader_module) {
        vk::ShaderModuleCreateInfo create_info{
            .sType = vk::StructureType::eShaderModuleCreateInfo,
            .codeSize = code.size(),
            .pCode = reinterpret_cast<const uint32_t*>(code.data()) // leverage that vector already aligns data to worst-case alignment
        };
        
        try {
            *shader_module = vk::raii::ShaderModule(ve_device.getDevice(), create_info);
        } catch (const std::exception& e) {
            throw std::runtime_error("failed to create shader module: " + std::string(e.what()));
        }
    }
}