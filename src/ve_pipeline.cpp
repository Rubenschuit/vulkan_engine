#include "ve_pipeline.hpp"
#include "ve_device.hpp"

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

    VePipeline::~VePipeline() {}

    PipelineConfigInfo VePipeline::defaultPipelineConfigInfo() {
        PipelineConfigInfo config_info{};
        config_info.dynamic_state_enables = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        config_info.dynamic_state_info = vk::PipelineDynamicStateCreateInfo{
            .dynamicStateCount = static_cast<uint32_t>(config_info.dynamic_state_enables.size()),
            .pDynamicStates = config_info.dynamic_state_enables.data()
        };
        config_info.viewport_info = { .viewportCount = 1, .scissorCount = 1 };

        config_info.input_assembly_info = {
            .sType = vk::StructureType::ePipelineInputAssemblyStateCreateInfo,
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = VK_FALSE
        };
        config_info.rasterization_info = {
            .sType = vk::StructureType::ePipelineRasterizationStateCreateInfo,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = vk::PolygonMode::eFill,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eClockwise,
            .depthBiasEnable = VK_FALSE,
            .lineWidth = 1.0f,
            .depthBiasConstantFactor = 0.0f,
            .depthBiasClamp = 0.0f,
            .depthBiasSlopeFactor = 0.0f
        };
        config_info.multisample_info = {
            .sType = vk::StructureType::ePipelineMultisampleStateCreateInfo,
            .sampleShadingEnable = VK_FALSE,
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .minSampleShading = 1.0f,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE
        };
        config_info.color_blend_attachment = {
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = vk::BlendFactor::eOne,
            .dstColorBlendFactor = vk::BlendFactor::eZero,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eZero,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | 
                              vk::ColorComponentFlagBits::eG | 
                              vk::ColorComponentFlagBits::eB | 
                              vk::ColorComponentFlagBits::eA
        };
        config_info.color_blend_info = {
            .sType = vk::StructureType::ePipelineColorBlendStateCreateInfo,
            .logicOpEnable = VK_FALSE,
            .logicOp = vk::LogicOp::eCopy,
            .attachmentCount = 1,
            .pAttachments = &config_info.color_blend_attachment
        };    
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
        file.read(buffer.data(), static_cast<std::streamsize>(file_size));
        file.close();
        return buffer;
    }

    void VePipeline::createGraphicsPipeline(
            const std::string& vert_file_path, 
            const std::string& frag_file_path, 
            const PipelineConfigInfo& config_info) {

        
        //assert(config_info.renderPass != nullptr && "Cannot create graphics pipeline: no renderPass provided in config_info");
        auto vert_shader_code = readFile(vert_file_path);
        auto frag_shader_code = readFile(frag_file_path);

        createShaderModule(vert_shader_code, &vert_shader_module);
        createShaderModule(frag_shader_code, &frag_shader_module);

        vk::PipelineShaderStageCreateInfo shader_stages[2] = {
            {
                .sType = vk::StructureType::ePipelineShaderStageCreateInfo,
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = *vert_shader_module,
                .pName = "main",
                .pSpecializationInfo = nullptr
            },
            {
                .sType = vk::StructureType::ePipelineShaderStageCreateInfo,
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = *frag_shader_module,
                .pName = "main",
                .pSpecializationInfo = nullptr
            }
        };
        vk::PipelineVertexInputStateCreateInfo vertex_input_info{
            .sType = vk::StructureType::ePipelineVertexInputStateCreateInfo,
            .vertexBindingDescriptionCount = 0, // for now, vertex input is hardcoded in the vertex shader
            .pVertexBindingDescriptions = nullptr,
            .vertexAttributeDescriptionCount = 0,
            .pVertexAttributeDescriptions = nullptr
        };

        vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{ 
            .colorAttachmentCount = 1, 
            .pColorAttachmentFormats = &config_info.color_format,
        };
        vk::GraphicsPipelineCreateInfo pipeline_info{
            .sType = vk::StructureType::eGraphicsPipelineCreateInfo,
            .pNext = &pipelineRenderingCreateInfo,
            .stageCount = 2,
            .pStages = shader_stages,
            .pVertexInputState = &vertex_input_info,
            .pInputAssemblyState = &config_info.input_assembly_info,
            .pViewportState = &config_info.viewport_info,
            .pRasterizationState = &config_info.rasterization_info,
            .pMultisampleState = &config_info.multisample_info,
            .pColorBlendState = &config_info.color_blend_info,
            .pDynamicState = &config_info.dynamic_state_info,
            .layout = config_info.pipeline_layout,
            .renderPass = nullptr,
            .subpass = config_info.subpass,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1
        };
        assert(config_info.pipeline_layout != nullptr && "Cannot create graphics pipeline: no pipelineLayout provided in config_info");

        std::cout << "Vertex shader code size: " << vert_shader_code.size() << " bytes" << std::endl;
        std::cout << "Fragment shader code size: " << frag_shader_code.size() << " bytes" << std::endl;
        
        graphics_pipeline = vk::raii::Pipeline{ve_device.getDevice(), nullptr, pipeline_info};
    
        
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