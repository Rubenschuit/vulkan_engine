#include "pch.hpp"
#include "core/ve_window.hpp"

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
	// Ensure GLFW scales window sizes appropriately on HiDPI/Retina displays
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

	// Our m_width/m_height are desired framebuffer pixel sizes.
	// GLFW window sizes are specified in logical points, so convert using the
	// monitor content scale before creating the window.
	float xscale = 1.0f, yscale = 1.0f;
	if (GLFWmonitor* primary = glfwGetPrimaryMonitor()) {
		glfwGetMonitorContentScale(primary, &xscale, &yscale);
	}
	if (xscale <= 0.0f) xscale = 1.0f;
	if (yscale <= 0.0f) yscale = 1.0f;

	int win_width_points = static_cast<int>(static_cast<float>(m_width) / xscale + 0.5f);
	int win_height_points = static_cast<int>(static_cast<float>(m_height) / yscale + 0.5f);
	m_window = glfwCreateWindow(win_width_points, win_height_points, m_window_name.c_str(), nullptr, nullptr);
	if (!m_window) {
		glfwTerminate();
		throw std::runtime_error("Failed to create GLFW window");
	}
	glfwSetWindowUserPointer(m_window, this);
	glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);

	// Initialize stored size to the actual framebuffer size (in pixels)
	int fb_w = 0, fb_h = 0;
	glfwGetFramebufferSize(m_window, &fb_w, &fb_h);
	m_width = fb_w;
	m_height = fb_h;
}
} // namespace ve