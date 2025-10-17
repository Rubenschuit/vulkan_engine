#pragma once

#include "ve_game_object.hpp"
#include "ve_window.hpp"
#include "ve_camera.hpp"
// Forward declaration to avoid heavy coupling
namespace ve { class ParticleSystem; }

namespace ve {
	class InputController {
	public:
		struct InputActions {
			bool reset_particles = false;
			int set_mode = 0; // 1..4 when pressed, 0 = no change
			bool reset_disc = false; // G key toggles disc reset mode
		};
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
			int reset_camera = GLFW_KEY_R;
			int reset_particles = GLFW_KEY_F;
			int reset_disc_toggle = GLFW_KEY_G;
			int mode1 = GLFW_KEY_1;
			int mode2 = GLFW_KEY_2;
			int mode3 = GLFW_KEY_3;
			int mode4 = GLFW_KEY_4;
		};

		InputController(VeWindow& window);
		~InputController();

		InputController(const InputController&) = delete;
		InputController& operator=(const InputController&) = delete;

		InputActions processInput(float delta_time, VeCamera& camera);

	private:
		void processMouseMovement(double xpos, double ypos);
		void handleMouseToggle();

        GLFWwindow* m_window{nullptr};
		//std::unordered_map<uint32_t, VeGameObject>& game_objects;

		KeyMappings m_key_mappings{};
		double m_last_x = 0.0;
		double m_last_y = 0.0;
		const float NORMAL_SPEED = 5.0f;
		const float SPRINT_SPEED = 15.0f;
		float m_movement_speed = NORMAL_SPEED;
		float m_look_speed = 2.0f;
		float m_mouse_sensitivity = 40.0f;
		float m_yaw_delta = 0.0f;
		float m_pitch_delta = 0.0f;

		// Mouse-look toggle state
		bool m_mouse_look_enabled = true;
		int m_prev_toggle_state = GLFW_RELEASE;
		int m_prev_reset_state = GLFW_RELEASE;
		int m_prev_g_state = GLFW_RELEASE;
		int m_prev_mode1 = GLFW_RELEASE;
		int m_prev_mode2 = GLFW_RELEASE;
		int m_prev_mode3 = GLFW_RELEASE;
		int m_prev_mode4 = GLFW_RELEASE;
	};
}