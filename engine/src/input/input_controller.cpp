#include "pch.hpp"
#include "input/input_controller.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

namespace ve {

InputController::InputController(VeWindow& ve_window, EventBus& event_bus, bool free_cursor_on_start)
	: m_event_bus(event_bus) {
	m_window = ve_window.getGLFWwindow();
	assert(m_window != nullptr && "InputController: GLFWwindow is null");
	m_user_editor_mode = free_cursor_on_start;
	m_cursor_locked = !free_cursor_on_start;
	if (free_cursor_on_start) {
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		return;
	}
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

void InputController::setMouseLook(bool enabled) {
	m_mouse_look = enabled;
	applyCursorMode();
}

void InputController::setUiKeyboardState(bool text_input, bool ui_takes_escape) {
	m_ui_text_input = text_input;
	m_ui_takes_escape = ui_takes_escape;
}

void InputController::applyCursorMode() {
	if (!effectiveEditorMode())
		m_mouse_look = false;
	bool locked = cursorLocked();
	if (locked == m_cursor_locked)
		return;
	m_cursor_locked = locked;
	if (locked) {
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
	if (m_mouse_look && (glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) != GLFW_PRESS
	                     || !glfwGetWindowAttrib(m_window, GLFW_FOCUSED)))
		setMouseLook(false);

	const bool keys_free = !m_ui_text_input;

	// Tab toggles the user's intended mode. While a cursor capture is held
	// the effective mode stays editor; toggling still updates intent for
	// when the capture is released.
	{
		int cur_btn = glfwGetKey(m_window, m_key_mappings.toggle_mouse_look);
		if (keys_free && cur_btn == GLFW_PRESS && m_prev_toggle_state == GLFW_RELEASE) {
			m_user_editor_mode = !m_user_editor_mode;
			applyCursorMode();
		}
		m_prev_toggle_state = cur_btn;
	}

	// Escape: close app in user-game-mode, return to user-game-mode otherwise.
	{
		int cur_escape = glfwGetKey(m_window, GLFW_KEY_ESCAPE);
		if (keys_free && !m_ui_takes_escape && cur_escape == GLFW_PRESS && m_prev_escape_state == GLFW_RELEASE) {
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
	actions.mouse_look_enabled = cursorLocked();

	// Ctrl belongs to the editor's shortcuts (Ctrl+D, Ctrl+G), not the camera
	const bool ctrl = glfwGetKey(m_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
		|| glfwGetKey(m_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
	const bool camera_keys = keys_free && !(editor && ctrl);
	auto held = [&](int key) { return camera_keys && glfwGetKey(m_window, key) == GLFW_PRESS; };

	if (held(m_key_mappings.look_left))
		actions.look_yaw -= 1.0f;
	if (held(m_key_mappings.look_right))
		actions.look_yaw += 1.0f;
	if (held(m_key_mappings.look_up))
		actions.look_pitch += 1.0f;
	if (held(m_key_mappings.look_down))
		actions.look_pitch -= 1.0f;

	if (cursorLocked()) {
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

	actions.sprint = held(GLFW_KEY_LEFT_SHIFT);
	actions.jump = held(GLFW_KEY_SPACE);

	if (held(m_key_mappings.move_forward))
		actions.move_forward += 1.0f;
	if (held(m_key_mappings.move_backward))
		actions.move_forward -= 1.0f;
	if (held(m_key_mappings.move_left))
		actions.move_right -= 1.0f;
	if (held(m_key_mappings.move_right))
		actions.move_right += 1.0f;
	// Preserves the legacy mapping where C reduces world-Z and SPACE raises it.
	if (held(m_key_mappings.move_up))
		actions.move_up -= 1.0f;
	if (held(m_key_mappings.move_down))
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

		if (fire && keys_free)
			m_event_bus.emitImmediate(InputActionEvent{action.binding.name, action.binding.value});
	}
}

} // namespace ve
