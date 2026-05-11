// Maps raw GLFW input to a per-frame InputActions snapshot and dispatches 
// registered actions via EventBus.
#pragma once
#include "ve_export.hpp"
#include "platform/ve_window.hpp"
#include "input/input_action.hpp"

#include <deque>

namespace ve {

class EventBus;

struct RegisteredAction {
	ActionBinding binding;
	int prev_state = GLFW_RELEASE;
};

class VENGINE_API InputController {
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

	void processInput(float delta_time);
	const InputActions& getActions() const { return m_current_actions; }

	void setEventBus(EventBus* bus);
	void registerAction(ActionBinding&& binding);
	const std::deque<RegisteredAction>& getRegisteredActions() const;
	bool isEditorMode() const;

private:
	GLFWwindow* m_window{nullptr};

	KeyMappings m_key_mappings{};
	double m_last_x = 0.0;
	double m_last_y = 0.0;

	bool m_mouse_look_enabled = true;
	int m_prev_toggle_state = GLFW_RELEASE;
	int m_prev_escape_state = GLFW_RELEASE;

	EventBus* m_event_bus = nullptr;
	std::deque<RegisteredAction> m_actions;
	InputActions m_current_actions{};
};

} // namespace ve