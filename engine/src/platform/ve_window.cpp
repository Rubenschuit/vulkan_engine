#include "pch.hpp"
#include "platform/ve_window.hpp"
#include "utils/ve_log.hpp"

#include <algorithm>
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
	VE_LOGI("Framebuffer resized to " + std::to_string(width) + "x" + std::to_string(height));
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

	// Seed the windowed-geometry cache so the first transition to/from a non-windowed
	// mode has a real position/size to restore to.
	rememberWindowedGeometry();
}

void VeWindow::rememberWindowedGeometry() {
	glfwGetWindowPos(m_window, &m_windowed_x, &m_windowed_y);
	glfwGetWindowSize(m_window, &m_windowed_width, &m_windowed_height);
}

GLFWmonitor* VeWindow::getCurrentMonitor() const {
	int wx = 0, wy = 0, ww = 0, wh = 0;
	glfwGetWindowPos(m_window, &wx, &wy);
	glfwGetWindowSize(m_window, &ww, &wh);
	int cx = wx + ww / 2;
	int cy = wy + wh / 2;

	int count = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&count);
	for (int i = 0; i < count; ++i) {
		int mx = 0, my = 0;
		glfwGetMonitorPos(monitors[i], &mx, &my);
		const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
		if (!mode)
			continue;
		if (cx >= mx && cx < mx + mode->width && cy >= my && cy < my + mode->height)
			return monitors[i];
	}
	return glfwGetPrimaryMonitor();
}

GLFWmonitor* VeWindow::resolveTargetMonitor() const {
	if (m_target_monitor_index >= 0) {
		int count = 0;
		GLFWmonitor** monitors = glfwGetMonitors(&count);
		if (m_target_monitor_index < count)
			return monitors[m_target_monitor_index];
	}
	return getCurrentMonitor();
}

std::vector<VeWindow::MonitorInfo> VeWindow::getMonitors() const {
	std::vector<MonitorInfo> result;
	int count = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&count);
	result.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		MonitorInfo info{};
		info.index = i;
		const char* name = glfwGetMonitorName(monitors[i]);
		info.name = name ? name : "";
		glfwGetMonitorPos(monitors[i], &info.x, &info.y);
		if (auto* m = glfwGetVideoMode(monitors[i])) {
			info.width = m->width;
			info.height = m->height;
			info.refresh_rate = m->refreshRate;
		}
		result.push_back(std::move(info));
	}
	return result;
}

std::vector<VeWindow::VideoMode> VeWindow::getVideoModes(int monitor_index) const {
	std::vector<VideoMode> result;
	int count = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&count);
	if (monitor_index < 0 || monitor_index >= count)
		return result;
	int mode_count = 0;
	const GLFWvidmode* modes = glfwGetVideoModes(monitors[monitor_index], &mode_count);
	if (!modes)
		return result;
	result.reserve(static_cast<size_t>(mode_count));
	for (int i = 0; i < mode_count; ++i) {
		VideoMode vm{modes[i].width, modes[i].height, modes[i].refreshRate};
		// GLFW returns modes sorted by bit depth too; dedupe identical (w, h, hz).
		if (std::find(result.begin(), result.end(), vm) == result.end())
			result.push_back(vm);
	}
	return result;
}

int VeWindow::getResolvedMonitorIndex() const {
	GLFWmonitor* target = resolveTargetMonitor();
	int count = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&count);
	for (int i = 0; i < count; ++i)
		if (monitors[i] == target)
			return i;
	return 0;
}

void VeWindow::setMonitor(int monitor_index) {
	m_target_monitor_index = monitor_index;
	if (m_window_mode != WindowMode::Windowed)
		applyMode(m_window_mode);
}

void VeWindow::setVideoMode(VideoMode mode) {
	m_target_video_mode = mode;
	if (m_window_mode == WindowMode::Fullscreen)
		applyMode(m_window_mode);
}

void VeWindow::setWindowMode(WindowMode mode) {
	if (mode == m_window_mode)
		return;
	if (m_window_mode == WindowMode::Windowed)
		rememberWindowedGeometry();
	applyMode(mode);
	m_window_mode = mode;
}

void VeWindow::applyMode(WindowMode mode) {
	GLFWmonitor* monitor = resolveTargetMonitor();
	const GLFWvidmode* desktop = monitor ? glfwGetVideoMode(monitor) : nullptr;

	switch (mode) {
		case WindowMode::Windowed: {
			glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
			glfwSetWindowMonitor(m_window, nullptr,
				m_windowed_x, m_windowed_y,
				m_windowed_width, m_windowed_height,
				GLFW_DONT_CARE);
			VE_LOGI("Window mode: Windowed (" << m_windowed_width << "x" << m_windowed_height << ")");
			break;
		}
		case WindowMode::Borderless: {
			if (!monitor || !desktop) {
				VE_LOGE("Borderless requested but no monitor found");
				return;
			}
			int mx = 0, my = 0;
			glfwGetMonitorPos(monitor, &mx, &my);
			glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_FALSE);
			glfwSetWindowMonitor(m_window, nullptr,
				mx, my, desktop->width, desktop->height,
				GLFW_DONT_CARE);
			VE_LOGI("Window mode: Borderless (" << desktop->width << "x" << desktop->height << ")");
			break;
		}
		case WindowMode::Fullscreen: {
			if (!monitor || !desktop) {
				VE_LOGE("Fullscreen requested but no monitor found");
				return;
			}
			int width  = m_target_video_mode.width  > 0 ? m_target_video_mode.width  : desktop->width;
			int height = m_target_video_mode.height > 0 ? m_target_video_mode.height : desktop->height;
			int rate   = m_target_video_mode.refresh_rate > 0 ? m_target_video_mode.refresh_rate : desktop->refreshRate;
			glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
			glfwSetWindowMonitor(m_window, monitor, 0, 0, width, height, rate);
			VE_LOGI("Window mode: Fullscreen (" << width << "x" << height << "@" << rate << "Hz)");
			break;
		}
	}
}

} // namespace ve
