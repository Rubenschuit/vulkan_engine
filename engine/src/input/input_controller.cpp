#include "pch.hpp"
#include "input/input_controller.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

namespace ve {

InputController::InputController(VeWindow& ve_window, EventBus& event_bus)
	: m_event_bus(event_bus) {
	m_window = ve_window.getGLFWwindow();
	assert(m_window != nullptr && "InputController: GLFWwindow is null");
	// Initial state: game mode (cursor disabled, raw motion on).
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
	return effectiveEditorMode();
}

void InputController::setEditorMode(bool enabled) {
	m_user_editor_mode = enabled;
	applyCursorMode();
}

void InputController::applyCursorMode() {
	bool editor = effectiveEditorMode();
	if (editor == m_last_applied_editor_mode)
		return;
	m_last_applied_editor_mode = editor;
	if (editor) {
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		if (glfwRawMouseMotionSupported())
			glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
	} else {
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		if (glfwRawMouseMotionSupported())
			glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
		glfwGetCursorPos(m_window, &m_last_x, &m_last_y);
	}
}

InputController::CursorCaptureToken InputController::acquireCursor() {
	++m_cursor_capture_count;
	applyCursorMode();
	return CursorCaptureToken{this};
}

void InputController::releaseCursorInternal() {
	if (m_cursor_capture_count > 0)
		--m_cursor_capture_count;
	applyCursorMode();
}

InputController::CursorCaptureToken::CursorCaptureToken(CursorCaptureToken&& other) noexcept
	: m_owner(other.m_owner) {
	other.m_owner = nullptr;
}

InputController::CursorCaptureToken& InputController::CursorCaptureToken::operator=(CursorCaptureToken&& other) noexcept {
	if (this != &other) {
		release();
		m_owner = other.m_owner;
		other.m_owner = nullptr;
	}
	return *this;
}

InputController::CursorCaptureToken::~CursorCaptureToken() {
	release();
}

void InputController::CursorCaptureToken::release() {
	if (m_owner) {
		m_owner->releaseCursorInternal();
		m_owner = nullptr;
	}
}

void InputController::processInput(float /*delta_time*/) {
	// Tab toggles the user's intended mode. While a cursor capture is held
	// the effective mode stays editor; toggling still updates intent for
	// when the capture is released.
	{
		int cur_btn = glfwGetKey(m_window, m_key_mappings.toggle_mouse_look);
		if (cur_btn == GLFW_PRESS && m_prev_toggle_state == GLFW_RELEASE) {
			m_user_editor_mode = !m_user_editor_mode;
			applyCursorMode();
		}
		m_prev_toggle_state = cur_btn;
	}

	// Escape: close app in user-game-mode, return to user-game-mode otherwise.
	{
		int cur_escape = glfwGetKey(m_window, GLFW_KEY_ESCAPE);
		if (cur_escape == GLFW_PRESS && m_prev_escape_state == GLFW_RELEASE) {
			if (!m_user_editor_mode) {
				glfwSetWindowShouldClose(m_window, true);
			} else {
				m_user_editor_mode = false;
				applyCursorMode();
			}
		}
		m_prev_escape_state = cur_escape;
	}

	bool editor = effectiveEditorMode();
	InputActions actions{};
	actions.mouse_look_enabled = !editor;

	if (glfwGetKey(m_window, m_key_mappings.look_left) == GLFW_PRESS)
		actions.look_yaw -= 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.look_right) == GLFW_PRESS)
		actions.look_yaw += 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.look_up) == GLFW_PRESS)
		actions.look_pitch += 1.0f;
	if (glfwGetKey(m_window, m_key_mappings.look_down) == GLFW_PRESS)
		actions.look_pitch -= 1.0f;

	if (!editor) {
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
		if (action.binding.context == InputContext::GameMode && editor)
			continue;
		if (action.binding.context == InputContext::EditorMode && !editor)
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
