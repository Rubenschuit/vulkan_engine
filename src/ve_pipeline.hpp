# pragma once

#include <string>
#include <vector>

namespace ve {
    class VePipeline {
    public:
        VePipeline(const std::string& vert_file_path, const std::string& frag_file_path);

        ~VePipeline();

        // Prevent copying
        VePipeline(const VePipeline&) = delete;
        VePipeline& operator=(const VePipeline&) = delete;

    private:
        static std::vector<char> readFile(const std::string& file_path);

        void createGraphicsPipeline(const std::string& vert_file_path, const std::string& frag_file_path);
    };
}