#include "ve_pipeline.hpp"

#include <fstream>
#include <stdexcept>
#include <iostream>

namespace ve {
    VePipeline::VePipeline(const std::string& vert_file_path, const std::string& frag_file_path) {
        createGraphicsPipeline(vert_file_path, frag_file_path);
    }

    VePipeline::~VePipeline() {
        // Cleanup code for the pipeline would go here
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

    void VePipeline::createGraphicsPipeline(const std::string& vert_file_path, const std::string& frag_file_path) {
        auto vert_shader_code = readFile(vert_file_path);
        auto frag_shader_code = readFile(frag_file_path);

        std::cout << "Vertex shader code size: " << vert_shader_code.size() << " bytes" << std::endl;
        std::cout << "Fragment shader code size: " << frag_shader_code.size() << " bytes" << std::endl;
    }
}