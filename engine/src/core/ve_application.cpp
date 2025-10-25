#include "core/ve_application.hpp"
#include "core/ve_window.hpp"
#include "core/ve_device.hpp"
#include "ui/imgui_layer.hpp"
#include "core/ve_buffer.hpp"
#include "input/input_controller.hpp"
#include "game/ve_camera.hpp"
#include "utils/ve_log.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <format>

namespace ve {

VeApplication::VeApplication() 
	: m_ve_window(WIDTH, HEIGHT, APP_NAME),
	  m_ve_device(m_ve_window),
	  m_ve_renderer(m_ve_device, m_ve_window),
	  m_input_controller(m_ve_window),
	  m_camera(glm::vec3{20.0f, 20.0f, 20.0f}, glm::vec3{0.0f, 0.0f, 1.0f}) {
}

void VeApplication::run() {
	VE_LOGI("VeApplication::run starting. Window=" + std::to_string(m_ve_window.getWidth()) + "x" + std::to_string(m_ve_window.getHeight()));

	// Main loop
	while (!m_ve_window.shouldClose()) {
		m_ve_window.pollEvents();

		if (!m_ve_renderer.beginFrame())
			continue;

		VeFrameInfo frame_info = update();
		render(frame_info);		

		m_ve_renderer.endFrame(frame_info.command_buffer);
	}

	// Ensure device is idle before destroying resources
	m_ve_device.getDevice().waitIdle();
	// log average fps and frametime over entire run currently these get reset on window resize
	VE_LOGI("VeApplication::run finished. Average FPS: " << (m_fps_frame_count / (m_sum_frame_ms / 1000.0f)));
	VE_LOGI("VeApplication::run finished. Average Frame Time: " << (m_sum_frame_ms / m_fps_frame_count) << " ms");
}

// Updates the camera view and projection matrices if state changed
void VeApplication::updateCamera() {
	// Recompute camera view once per frame if needed
	m_camera.updateIfDirty();
	// If swapchain aspect changed (window resize), refresh camera projection
	float aspect = m_ve_renderer.getExtentAspectRatio();
	if (aspect > 0.0f && std::abs(aspect - m_last_aspect) > std::numeric_limits<float>::epsilon()) {
		m_last_aspect = aspect;
		m_camera.setPerspective(m_fov, m_last_aspect, m_near_plane, m_far_plane);
	}
}

// Updates the camera and uniform buffer object once per frame
void VeApplication::updateUniformBuffer(uint32_t current_frame, UniformBufferObject& ubo) {
	ubo.view = m_camera.getView();
	ubo.proj = m_camera.getProj();
	m_uniform_buffers[current_frame]->writeToBuffer(&ubo);
	// No flush required with MEMORY_PROPERTY_HOST_COHERENT
}

// Print FPS and frame time to window title every 100 ms
void VeApplication::updateWindowTitle() {
	updateFPSStats();
	
	if (shouldUpdateWindowTitle()) {
		auto now = clock::now();
		auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fps_window_start).count();
		
		double fps = (window_ms > 0) ? (1000.0 * static_cast<double>(m_fps_frame_count) / static_cast<double>(window_ms)) : 0.0;
		double avg_ms = (m_fps_frame_count > 0) ? (m_sum_frame_ms / static_cast<double>(m_fps_frame_count)) : 0.0;
		
		std::string title = formatWindowTitle(fps, avg_ms);
		glfwSetWindowTitle(m_ve_window.getGLFWwindow(), title.c_str());
		
		// Reset window counters
		m_fps_frame_count = 0;
		m_sum_frame_ms = 0.0;
		m_fps_window_start = now;
	}
}

void VeApplication::updateFrameTime() {
	auto now = clock::now();
	m_frame_time = std::chrono::duration<float, std::chrono::seconds::period>(now - m_last_frame_time).count();
	m_last_frame_time = now;
	
	// Clamp to avoid large physics steps after stalls (e.g., window resize)
	const float max_dt = 1.0f / 30.0f; // ~33ms
	if (m_frame_time < 0.0f)
		m_frame_time = 0.0f;
	if (m_frame_time > max_dt)
		m_frame_time = max_dt;
	m_frame_time *= 2; // speed up time
}

void VeApplication::updateFPSStats() {
	m_sum_frame_ms += m_frame_time * 1000.0; // Convert to milliseconds
	m_fps_frame_count++;
}

bool VeApplication::shouldUpdateWindowTitle() const {
	auto now = clock::now();
	auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fps_window_start);
	return window_ms >= WINDOW_TITLE_UPDATE_INTERVAL;
}

std::string VeApplication::formatWindowTitle(double fps, double avg_ms) const {
	
#ifdef NDEBUG
	const char* mode_str = "Release";
#else
	const char* mode_str = "Debug";
#endif

	return std::format("Vulkan Engine! -- {} mode          FPS {}   {:.2f} ms", 
		mode_str, static_cast<int>(fps), avg_ms);
}

} // namespace ve