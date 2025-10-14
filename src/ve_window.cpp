#include "pch.hpp"
#include "ve_window.hpp"

#include <stdexcept>

namespace ve {
	VeWindow::VeWindow(int width, int height, std::string name) : m_window_name(name), m_width(width), m_height(height)  {
		initWindow();
	}

	VeWindow::~VeWindow() {
		glfwDestroyWindow(m_window);
		glfwTerminate();
	}

	void VeWindow::framebufferResizeCallback(GLFWwindow* glfw_window, int width, int height) {
		auto ve_window = reinterpret_cast<VeWindow*>(glfwGetWindowUserPointer(glfw_window));
		ve_window->m_height = height;
		ve_window->m_width = width;
		ve_window->m_framebuffer_resized = true;
	}

	void VeWindow::initWindow() {
		glfwInit();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // No OpenGL context creation
		glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

		m_window = glfwCreateWindow(m_width, m_height, m_window_name.c_str(), nullptr, nullptr);
		glfwSetWindowUserPointer(m_window, this);
		glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);
	}
}