#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan_core.h>
#include <string>

namespace ve {  
    class VeWindow {
    public:
        VeWindow(int width, int height, std::string name);
        ~VeWindow();

        // Prevent copying, ensuring unique ownership of GLFWwindow
        VeWindow(const VeWindow&) = delete;
        VeWindow& operator=(const VeWindow&) = delete;

        void createWindowSurface(VkInstance instance, VkSurfaceKHR* surface);

        GLFWwindow* getGLFWwindow() const { return window; }
        int getWidth() const { return width; }
        int getHeight() const { return height; }

    private:
        void initWindow(int width, int height, const std::string name);

        GLFWwindow* window;
        std::string window_name;
        int width;
        int height;
    };
}
