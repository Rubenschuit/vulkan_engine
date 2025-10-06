
#include "pch.hpp"
#include "input_controller.hpp"


namespace ve {
	InputController::InputController(VeWindow& ve_window) {
		window = ve_window.getGLFWwindow();
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		if (glfwRawMouseMotionSupported())
   			glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
	}

	InputController::~InputController() {}

	void InputController::processInput(float delta_time, VeCamera& camera) {
		// Close window on double Escape key press
		if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
			glfwSetWindowShouldClose(window, true);
		}

		handleMouseToggle();

		// Keyboard look
		yaw_delta = 0.0f;
		pitch_delta = 0.0f;
		if (glfwGetKey(window, key_mappings.look_left) == GLFW_PRESS)
			yaw_delta   -= look_speed;
		if (glfwGetKey(window, key_mappings.look_right) == GLFW_PRESS)
			yaw_delta   += look_speed;
		if (glfwGetKey(window, key_mappings.look_up) == GLFW_PRESS)
			pitch_delta += look_speed;
		if (glfwGetKey(window, key_mappings.look_down) == GLFW_PRESS)
			pitch_delta -= look_speed;

		// Mouse look (only when enabled)
		if (mouse_look_enabled) {
			double xpos, ypos;
			glfwGetCursorPos(window, &xpos, &ypos);
			processMouseMovement(xpos, ypos);
		}
		// Apply look deltas
		camera.yawBy(yaw_delta * delta_time);
		camera.pitchBy(pitch_delta * delta_time);
		camera.updateIfDirty();

		// WASD movement input in the updated camera basis
		if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
			movement_speed = 15.0f; // Fast
		else
			movement_speed = 2.5f; // Normal
		auto movement_delta = movement_speed * delta_time;
		if (glfwGetKey(window, key_mappings.move_forward) == GLFW_PRESS)
			camera.moveForward(movement_delta);
		if (glfwGetKey(window, key_mappings.move_backward) == GLFW_PRESS)
			camera.moveForward(-movement_delta);
		if (glfwGetKey(window, key_mappings.move_left) == GLFW_PRESS)
			camera.moveRight(-movement_delta);
		if (glfwGetKey(window, key_mappings.move_right) == GLFW_PRESS)
			camera.moveRight(movement_delta);
		if (glfwGetKey(window, key_mappings.move_up) == GLFW_PRESS)
			camera.moveUpWorld(-movement_delta);
		if (glfwGetKey(window, key_mappings.move_down) == GLFW_PRESS)
			camera.moveUpWorld(movement_delta);
	}

	void InputController::processMouseMovement(double xpos, double ypos) {
		if (last_x == 0.0 && last_y == 0.0) {
			last_x = xpos;
			last_y = ypos;
		}

		float xoffset = static_cast<float>(xpos - last_x);
		float yoffset = static_cast<float>(last_y - ypos); // Reversed: y ranges bottom to top

		last_x = xpos;
		last_y = ypos;

		xoffset *= mouse_sensitivity;
		yoffset *= mouse_sensitivity;

		yaw_delta += glm::radians(xoffset); // yaw
		pitch_delta += glm::radians(yoffset); // pitch
	}

	// TODO: maybe use callback
	void InputController::handleMouseToggle() {
		// Toggle mouse-look on mouse button click (edge-triggered)
		int cur_btn = glfwGetKey(window, key_mappings.toggle_mouse_look);
		if (cur_btn == GLFW_PRESS && prev_toggle_state == GLFW_RELEASE) {
			mouse_look_enabled = !mouse_look_enabled;
			if (mouse_look_enabled) {
				glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
				if (glfwRawMouseMotionSupported())
					glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
				// Reset last positions to avoid jump on re-enable
				glfwGetCursorPos(window, &last_x, &last_y);
			} else {
				glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
				if (glfwRawMouseMotionSupported())
					glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
			}
		}
		prev_toggle_state = cur_btn;
	}
}