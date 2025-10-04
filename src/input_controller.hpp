#pragma once

#include "ve_game_object.hpp"
#include "ve_window.hpp"
#include "ve_camera.hpp"

namespace ve {
	class InputController {
	public:
		struct KeyMappings {
			int move_forward = GLFW_KEY_W;
			int move_backward = GLFW_KEY_S;
			int move_left = GLFW_KEY_A;
			int move_right = GLFW_KEY_D;
			int move_up = GLFW_KEY_C;
			int move_down = GLFW_KEY_SPACE;

			int look_up = GLFW_KEY_UP;
			int look_down = GLFW_KEY_DOWN;
			int look_left = GLFW_KEY_LEFT;
			int look_right = GLFW_KEY_RIGHT;

			int toggle_mouse_look = GLFW_KEY_TAB;
		};

		InputController(VeWindow& window);
		~InputController();

		InputController(const InputController&) = delete;
		InputController& operator=(const InputController&) = delete;

		void processInput(float delta_time, VeCamera& camera);

	private:
		void processMouseMovement(double xpos, double ypos);
		void handleMouseToggle();

		VeWindow& ve_window;
        GLFWwindow* window{nullptr};
		//std::unordered_map<uint32_t, VeGameObject>& game_objects;

		KeyMappings key_mappings{};
		double last_x = 0.0;
		double last_y = 0.0;
		float movement_speed = 2.5f;
		float look_speed = 2.0f;
		float mouse_sensitivity = 45.0f;
		float yaw_delta = 0.0f;
		float pitch_delta = 0.0f;

		// Mouse-look toggle state
		bool mouse_look_enabled = true;
		int prev_toggle_state = GLFW_RELEASE;
	};
}