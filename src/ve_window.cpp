#include "ve_window.hpp"

namespace ve {
    VeWindow::VeWindow(int width, int height, std::string name) {
        initWindow(width, height, name);
    }

    VeWindow::~VeWindow() {
        glfwDestroyWindow(window);
        glfwTerminate();
    }

    void VeWindow::initWindow(int width, int height, const std::string name) {
        this->width = width;
        this->height = height;
        this->window_name = name;

        glfwInit();

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // No OpenGL context creation
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

        window = glfwCreateWindow(width, height, window_name.c_str(), nullptr, nullptr);
    }
}