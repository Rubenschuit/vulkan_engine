#include "pch.hpp"
#include "input/input_controller.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

namespace ve {

InputController::InputController(VeWindow& ve_window, EventBus& event_bus)
	: m_event_bus(event_bus) {
	m_window = ve_window.getGLFWwindow();
	assert(m_window != nullptr && "InputController: GLFWwindow is null");
	glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	if (glfwRawMouseMotionSupported())
		glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
}

InputController::~InputController() {}

void InputController::registerAction(ActionBinding&& binding) {
	m_actions.push_back(RegisteredAction{std::move(binding)});
}

const std::deque<RegisteredAction>& InputController::getRegisteredActions() const {
	return m_actions;
}

bool InputController::isEditorMode() const {
	return !m_mouse_look_enabled;
}

void InputController::processInput(float /*delta_time*/) {
	// Tab toggle between game mode and editor mode
	{
		int cur_btn = glfwGetKey(m_window, m_key_mappings.toggle_mouse_look);
		if (cur_btn == GLFW_PRESS && m_prev_toggle_state == GLFW_RELEASE) {
			m_mouse_look_enabled = !m_mouse_look_enabled;
			if (m_mouse_look_enabled) {
				glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
				if (glfwRawMouseMotionSupported())
					glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
				glfwGetCursorPos(m_window, &m_last_x, &m_last_y);
			} else {
				glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
				if (glfwRawMouseMotionSupported())
					glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
			}
		}
		m_prev_toggle_state = cur_btn;
	}

	// Escape: close app in game mode, return to game mode in editor mode
	{
		int cur_escape = glfwGetKey(m_window, GLFW_KEY_ESCAPE);
		if (cur_escape == GLFW_PRESS && m_prev_escape_state == GLFW_RELEASE) {
			if (m_mouse_look_enabled) {
				glfwSetWindowShouldClose(m_window, true);
			} else {
				m_mouse_look_enabled = true;
				glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
				if (glfwRawMouseMotionSupported())
					glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
				glfwGetCursorPos(m_window, &m_last_x, &m_last_y);
			}
		}
		m_prev_escape_state = cur_escape;
	}

	InputActions actions{};
	actions.mouse_look_enabled = m_mouse_look_enabled;

	if (glfwGetKey(m_window, m_key_mappings.look_left) == GLFW_PRESS)
		actions.look_yaw -= 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.look_right) == GLFW_PRESS)
		actions.look_yaw += 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.look_up) == GLFW_PRESS)
		actions.look_pitch += 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.look_down) == GLFW_PRESS)
		actions.look_pitch -= 1.0f;

	if (m_mouse_look_enabled) {
		double xpos, ypos;
		glfwGetCursorPos(m_window, &xpos, &ypos);
		// First-frame guard so mouse_dx/dy stays at zero before m_last_* is initialized.
		if (m_last_x == 0.0 && m_last_y == 0.0) {
			m_last_x = xpos;
			m_last_y = ypos;
		}
		actions.mouse_dx = static_cast<float>(xpos - m_last_x);
		actions.mouse_dy = static_cast<float>(m_last_y - ypos);
		m_last_x = xpos;
		m_last_y = ypos;
	}

	actions.sprint = (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);

	if (glfwGetKey(m_window, m_key_mappings.move_forward) == GLFW_PRESS)
		actions.move_forward += 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.move_backward) == GLFW_PRESS)
		actions.move_forward -= 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.move_left) == GLFW_PRESS)
		actions.move_right -= 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.move_right) == GLFW_PRESS)
		actions.move_right += 1.0f;
	// Preserves the legacy mapping where C reduces world-Z and SPACE raises it.
	if (glfwGetKey(m_window, m_key_mappings.move_up) == GLFW_PRESS)
		actions.move_up -= 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.move_down) == GLFW_PRESS)
		actions.move_up += 1.0f;

	m_current_actions = actions;

	// Registered actions
	for (auto& action : m_actions) {
		if (action.binding.context == InputContext::GameMode && !m_mouse_look_enabled)
			continue;
		if (action.binding.context == InputContext::EditorMode && m_mouse_look_enabled)
			continue;

		int cur = glfwGetKey(m_window, action.binding.key);
		bool fire = false;
		if (action.binding.trigger == TriggerType::OnPress)
			fire = (cur == GLFW_PRESS && action.prev_state == GLFW_RELEASE);
		else
			fire = (cur == GLFW_PRESS);
		action.prev_state = cur;

		if (fire)
			m_event_bus.emitImmediate(InputActionEvent{action.binding.name, action.binding.value});
	}
}

} // namespace ve
