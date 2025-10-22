/* VeWindow is responsible for creating and managing a GLFW window. */
#pragma once
#include "ve_export.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_core.h>
#include <string>

namespace ve {

class VENGINE_API VeWindow {
public:
	VeWindow(int width, int height, std::string name);
	~VeWindow();

	// Prevent copying, ensuring unique ownership of GLFWwindow
	VeWindow(const VeWindow&) = delete;
	VeWindow& operator=(const VeWindow&) = delete;



	GLFWwindow* getGLFWwindow() const { return m_window; }
	int getWidth() const { return m_width; }
	int getHeight() const { return m_height; }
	vk::Extent2D getExtent() const { return {static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height)}; }
	bool wasWindowResized() const { return m_framebuffer_resized; }
	void resetWindowResizedFlag() { m_framebuffer_resized = false; }

private:
	void initWindow();
	static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

	GLFWwindow* m_window;
	std::string m_window_name;
	int m_width;
	int m_height;
	bool m_framebuffer_resized = false;
};
}
