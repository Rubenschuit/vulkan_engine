#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
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



		GLFWwindow* getGLFWwindow() const { return window; }
		int getWidth() const { return width; }
		int getHeight() const { return height; }
		vk::Extent2D getExtent() const { return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}; }
		bool wasWindowResized() const { return framebuffer_resized; }
		void resetWindowResizedFlag() { framebuffer_resized = false; }

	private:
		void initWindow();
		static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

		GLFWwindow* window;
		std::string window_name;
		int width;
		int height;
		bool framebuffer_resized = false;
	};
}
