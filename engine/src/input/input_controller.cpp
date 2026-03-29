#include "pch.hpp"
#include "input/input_controller.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

namespace ve {

InputController::InputController(VeWindow& ve_window) {
	m_window = ve_window.getGLFWwindow();
	assert(m_window != nullptr && "InputController: GLFWwindow is null");
	glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	if (glfwRawMouseMotionSupported())
		glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
}

InputController::~InputController() {}

void InputController::setEventBus(EventBus* bus) {
	m_event_bus = bus;
}

void InputController::registerAction(ActionBinding&& binding) {
	m_actions.push_back(RegisteredAction{std::move(binding)});
}

const std::deque<RegisteredAction>& InputController::getRegisteredActions() const {
	return m_actions;
}

bool InputController::isEditorMode() const {
	return !m_mouse_look_enabled;
}

void InputController::processInput(float delta_time, VeCamera& camera) {
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

	// Keyboard look
	m_yaw_delta = 0.0f;
	m_pitch_delta = 0.0f;
	if (glfwGetKey(m_window, m_key_mappings.look_left) == GLFW_PRESS)
		m_yaw_delta -= m_look_speed * delta_time;
	if (glfwGetKey(m_window, m_key_mappings.look_right) == GLFW_PRESS)
		m_yaw_delta += m_look_speed * delta_time;
	if (glfwGetKey(m_window, m_key_mappings.look_up) == GLFW_PRESS)
		m_pitch_delta += m_look_speed * delta_time;
	if (glfwGetKey(m_window, m_key_mappings.look_down) == GLFW_PRESS)
		m_pitch_delta -= m_look_speed * delta_time;

	// Mouse look (only in game mode)
	if (m_mouse_look_enabled) {
		double xpos, ypos;
		glfwGetCursorPos(m_window, &xpos, &ypos);
		processMouseMovement(xpos, ypos);
	}
	camera.yawBy(m_yaw_delta);
	camera.pitchBy(m_pitch_delta);
	camera.updateIfDirty();

	// WASD movement
	if (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
		m_movement_speed = SPRINT_SPEED;
	else
		m_movement_speed = NORMAL_SPEED;
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

		if (fire && m_event_bus)
			m_event_bus->emitImmediate(InputActionEvent{action.binding.name, action.binding.value});
	}
}

void InputController::processMouseMovement(double xpos, double ypos) {
	if (m_last_x == 0.0 && m_last_y == 0.0) {
		m_last_x = xpos;
		m_last_y = ypos;
	}

	float xoffset = static_cast<float>(xpos - m_last_x);
	float yoffset = static_cast<float>(m_last_y - ypos);

	m_last_x = xpos;
	m_last_y = ypos;

	xoffset *= m_mouse_sensitivity;
	yoffset *= m_mouse_sensitivity;

	m_yaw_delta += glm::radians(xoffset);
	m_pitch_delta += glm::radians(yoffset);
}

} // namespace ve
