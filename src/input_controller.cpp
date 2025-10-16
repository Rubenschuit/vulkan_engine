
#include "pch.hpp"
#include "input_controller.hpp"


namespace ve {
	InputController::InputController(VeWindow& ve_window) {
		m_window = ve_window.getGLFWwindow();
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		if (glfwRawMouseMotionSupported())
	   		glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
	}

	InputController::~InputController() {}

	void InputController::processInput(float delta_time, VeCamera& camera, ParticleSystem& particle_system) {
		// Close window on double Escape key press
		if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
			glfwSetWindowShouldClose(m_window, true);
		}

		handleMouseToggle();

		// Keyboard look
		m_yaw_delta = 0.0f;
		m_pitch_delta = 0.0f;
		if (glfwGetKey(m_window, m_key_mappings.look_left) == GLFW_PRESS)
			m_yaw_delta   -= m_look_speed;
		if (glfwGetKey(m_window, m_key_mappings.look_right) == GLFW_PRESS)
			m_yaw_delta   += m_look_speed;
		if (glfwGetKey(m_window, m_key_mappings.look_up) == GLFW_PRESS)
			m_pitch_delta += m_look_speed;
		if (glfwGetKey(m_window, m_key_mappings.look_down) == GLFW_PRESS)
			m_pitch_delta -= m_look_speed;

		// Mouse look (only when enabled)
		if (m_mouse_look_enabled) {
			double xpos, ypos;
			glfwGetCursorPos(m_window, &xpos, &ypos);
			processMouseMovement(xpos, ypos);
		}
		// Apply look deltas
		camera.yawBy(m_yaw_delta * delta_time);
		camera.pitchBy(m_pitch_delta * delta_time);
		camera.updateIfDirty();

		// WASD movement input in the updated camera basis
		if (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
			m_movement_speed = 15.0f; // Fast
		else
			m_movement_speed = 2.5f; // Normal
		auto movement_delta = m_movement_speed * delta_time;
		if (glfwGetKey(m_window, m_key_mappings.move_forward) == GLFW_PRESS)
			camera.moveForward(movement_delta);
		if (glfwGetKey(m_window, m_key_mappings.move_backward) == GLFW_PRESS)
			camera.moveForward(-movement_delta);
		if (glfwGetKey(m_window, m_key_mappings.move_left) == GLFW_PRESS)
			camera.moveRight(-movement_delta);
		if (glfwGetKey(m_window, m_key_mappings.move_right) == GLFW_PRESS)
			camera.moveRight(movement_delta);
		if (glfwGetKey(m_window, m_key_mappings.move_up) == GLFW_PRESS)
			camera.moveUpWorld(-movement_delta);
		if (glfwGetKey(m_window, m_key_mappings.move_down) == GLFW_PRESS)
			camera.moveUpWorld(movement_delta);


		if (glfwGetKey(m_window, m_key_mappings.reset_camera) == GLFW_PRESS) {
			camera.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
		}
		// Particle system input
		if (glfwGetKey(m_window, GLFW_KEY_F) == GLFW_PRESS) {
			particle_system.restart();
		}
	}

	void InputController::processMouseMovement(double xpos, double ypos) {
		if (m_last_x == 0.0 && m_last_y == 0.0) {
			m_last_x = xpos;
			m_last_y = ypos;
		}

		float xoffset = static_cast<float>(xpos - m_last_x);
		float yoffset = static_cast<float>(m_last_y - ypos); // Reversed: y ranges bottom to top

		m_last_x = xpos;
		m_last_y = ypos;

		xoffset *= m_mouse_sensitivity;
		yoffset *= m_mouse_sensitivity;

		m_yaw_delta += glm::radians(xoffset); // yaw
		m_pitch_delta += glm::radians(yoffset); // pitch
	}

	// TODO: maybe use callback
	void InputController::handleMouseToggle() {
		// Toggle mouse-look on mouse button click (edge-triggered)
		int cur_btn = glfwGetKey(m_window, m_key_mappings.toggle_mouse_look);
		if (cur_btn == GLFW_PRESS && m_prev_toggle_state == GLFW_RELEASE) {
			m_mouse_look_enabled = !m_mouse_look_enabled;
			if (m_mouse_look_enabled) {
				glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
				if (glfwRawMouseMotionSupported())
					glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
				// Reset last positions to avoid jump on re-enable
				glfwGetCursorPos(m_window, &m_last_x, &m_last_y);
			} else {
				glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
				if (glfwRawMouseMotionSupported())
					glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
			}
		}
		m_prev_toggle_state = cur_btn;
	}
}