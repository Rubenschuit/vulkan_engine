/* VeWindow is responsible for creating and managing a GLFW window. */
#pragma once
#include "ve_export.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_core.h>
#include <string>
#include <vector>

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

	bool shouldClose() const { return glfwWindowShouldClose(m_window); }
	void pollEvents() { glfwPollEvents(); }

	// On modern Windows a fullscreen-covering borderless window can hit DWM Independent
	// Flip: latency comparable to exclusive fullscreen, but alt-tab is slowish.
	// HDR keeps DWM in always-composed flip (CCCS/FP16 blend),
	// which disables Independent Flip: instant alt-tab.
	// VK_EXT_full_screen_exclusive would give us guaranteed exclusive presentation; not used.
	enum class WindowMode {
		Windowed,
		Borderless,
		Fullscreen,
	};
	void setWindowMode(WindowMode mode);
	WindowMode getWindowMode() const { return m_window_mode; }

	struct MonitorInfo {
		int index;
		std::string name;
		int x, y;          // virtual desktop position
		int width, height; // current desktop video mode size
		int refresh_rate;
	};
	struct VideoMode {
		int width;
		int height;
		int refresh_rate;
		bool operator==(const VideoMode& o) const {
			return width == o.width && height == o.height && refresh_rate == o.refresh_rate;
		}
	};

	std::vector<MonitorInfo> getMonitors() const;
	std::vector<VideoMode> getVideoModes(int monitor_index) const;
	int getResolvedMonitorIndex() const;
	VideoMode getTargetVideoMode() const { return m_target_video_mode; }

	// Set target monitor for Borderless/Fullscreen. In Windowed it is stored only.
	// Wayland note: even in Borderless/Fullscreen the compositor may ignore the choice.
	void setMonitor(int monitor_index);
	// Request a specific video mode for Fullscreen; cached for next entry.
	// {0,0,0} = use the monitor's current desktop mode.
	void setVideoMode(VideoMode mode);

private:
	void initWindow();
	static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
	GLFWmonitor* getCurrentMonitor() const;
	GLFWmonitor* resolveTargetMonitor() const;
	void rememberWindowedGeometry();
	void applyMode(WindowMode mode);

	GLFWwindow* m_window;
	std::string m_window_name;
	int m_width;
	int m_height;
	bool m_framebuffer_resized = false;

	WindowMode m_window_mode = WindowMode::Windowed;
	int m_windowed_x = 0;
	int m_windowed_y = 0;
	int m_windowed_width = 0;
	int m_windowed_height = 0;

	int m_target_monitor_index = -1;        // -1 = auto (monitor under window center)
	VideoMode m_target_video_mode{0, 0, 0}; // {0,0,0} = use monitor's desktop mode
};
}
