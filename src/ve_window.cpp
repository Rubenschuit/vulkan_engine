#include "ve_window.hpp"

#include <stdexcept>

namespace ve {
	VeWindow::VeWindow(int width, int height, std::string name) : window_name(name), width(width), height(height)  {
		initWindow();
	}

	VeWindow::~VeWindow() {
		glfwDestroyWindow(window);
		glfwTerminate();
	}

	void VeWindow::framebufferResizeCallback(GLFWwindow* glfw_window, int width, int height) {
		auto ve_window = reinterpret_cast<VeWindow*>(glfwGetWindowUserPointer(glfw_window));
		ve_window->height = height;
		ve_window->width = width;
		ve_window->framebuffer_resized = true;
	}

	void VeWindow::initWindow() {
		glfwInit();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // No OpenGL context creation
		glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

		window = glfwCreateWindow(width, height, window_name.c_str(), nullptr, nullptr);
		glfwSetWindowUserPointer(window, this);
		glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
	}
}