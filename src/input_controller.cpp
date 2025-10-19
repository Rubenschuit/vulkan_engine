#include "pch.hpp"
#include "input_controller.hpp"
#include "systems/particle_system.hpp"


namespace ve {
	InputController::InputController(VeWindow& ve_window) {
		m_window = ve_window.getGLFWwindow();
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		if (glfwRawMouseMotionSupported())
	   		glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
	}

	InputController::~InputController() {}

	InputController::InputActions InputController::processInput(float delta_time, VeCamera& camera) {
		InputActions actions{};
		// Close window on double Escape key press
		if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
			glfwSetWindowShouldClose(m_window, true);
		}

		// Handle mouse-look toggle (Tab). Also drive UI visibility from this state.
		{
			int cur_btn = glfwGetKey(m_window, m_key_mappings.toggle_mouse_look);
			if (cur_btn == GLFW_PRESS && m_prev_toggle_state == GLFW_RELEASE) {
				m_mouse_look_enabled = !m_mouse_look_enabled;
				actions.ui_toggle = true;
				// Update cursor modes according to the new state
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
			actions.ui_visible = !m_mouse_look_enabled;
		}

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
			m_movement_speed = SPRINT_SPEED; // Fast
		else
			m_movement_speed = NORMAL_SPEED; // Normal
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
		// Particle system input (edge-trigger on press)
		int cur_reset = glfwGetKey(m_window, m_key_mappings.reset_particles);
		if (cur_reset == GLFW_PRESS && m_prev_reset_state == GLFW_RELEASE) {
			actions.reset_particles = true;
		}
		m_prev_reset_state = cur_reset;

		// Toggle reset disc mode with G
		int cur_g = glfwGetKey(m_window, m_key_mappings.reset_disc_toggle);
		if (cur_g == GLFW_PRESS && m_prev_g_state == GLFW_RELEASE) {
			actions.reset_disc = true;
		}
		m_prev_g_state = cur_g;

		// Number keys 1..4 to set mode
		int cur_m1 = glfwGetKey(m_window, m_key_mappings.mode1);
		if (cur_m1 == GLFW_PRESS && m_prev_mode1 == GLFW_RELEASE) actions.set_mode = 1;
		m_prev_mode1 = cur_m1;
		int cur_m2 = glfwGetKey(m_window, m_key_mappings.mode2);
		if (cur_m2 == GLFW_PRESS && m_prev_mode2 == GLFW_RELEASE) actions.set_mode = 2;
		m_prev_mode2 = cur_m2;
		int cur_m3 = glfwGetKey(m_window, m_key_mappings.mode3);
		if (cur_m3 == GLFW_PRESS && m_prev_mode3 == GLFW_RELEASE) actions.set_mode = 3;
		m_prev_mode3 = cur_m3;
		int cur_m4 = glfwGetKey(m_window, m_key_mappings.mode4);
		if (cur_m4 == GLFW_PRESS && m_prev_mode4 == GLFW_RELEASE) actions.set_mode = 4;
		m_prev_mode4 = cur_m4;

		return actions;
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