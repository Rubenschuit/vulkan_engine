#pragma once
#include <ve_window.hpp>
#include <ve_pipeline.hpp>

namespace ve {
    class VeApp {
    public:
        VeApp();
        ~VeApp();

        static constexpr int WIDTH = 800;
        static constexpr int HEIGHT = 600;

        void run();

    private:
        VeWindow window{WIDTH, HEIGHT, "Vulkan Engine!"};
        VePipeline pipeline{"../shaders/simple_shader.vert.spv", "../shaders/simple_shader.frag.spv"};
    };
}
