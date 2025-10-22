#include "ve_application.hpp"

namespace ve {

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
	// Per-frame delta using steady clock
	auto now = clock::now();
	m_last_frame_ms = std::chrono::duration<double, std::milli>(now - m_last_frame_time).count();
	m_last_frame_time = now;

	// Accumulate
	m_sum_frame_ms += m_last_frame_ms;
	m_fps_frame_count++;
	auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fps_window_start).count();
	if (window_ms >= 100) { // update every 100 ms
		double fps = (window_ms > 0) ? (1000.0 * static_cast<double>(m_fps_frame_count) / static_cast<double>(window_ms)) : 0.0;
		double avg_ms = (m_fps_frame_count > 0) ? (m_sum_frame_ms / static_cast<double>(m_fps_frame_count)) : 0.0;
		char buf[128];
		// add release/debug mode with ifdef
#ifdef NDEBUG
		const char* mode_str = "Release";
#else
		const char* mode_str = "Debug";
#endif
		snprintf(buf, sizeof(buf), "Vulkan Engine! -- %s mode          FPS %d   %.2f ms",
					mode_str, static_cast<int>(fps), avg_ms
		);
		glfwSetWindowTitle(m_ve_window.getGLFWwindow(), buf);
		// Reset window counters
		m_fps_frame_count = 0;
		m_sum_frame_ms = 0.0;
		m_fps_window_start = now;
	}
}

} // namespace ve