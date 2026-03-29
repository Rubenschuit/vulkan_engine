#include "pch.hpp"
#include "input/input_action.hpp"

#include <GLFW/glfw3.h>

namespace ve {

std::string keyDisplayName(int glfw_key) {
	switch (glfw_key) {
		case GLFW_KEY_SPACE: return "Space";
		case GLFW_KEY_TAB: return "Tab";
		case GLFW_KEY_ESCAPE: return "Escape";
		case GLFW_KEY_ENTER: return "Enter";
		case GLFW_KEY_BACKSPACE: return "Backspace";
		case GLFW_KEY_DELETE: return "Delete";
		case GLFW_KEY_LEFT_SHIFT: return "Shift";
		case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
		case GLFW_KEY_LEFT_CONTROL: return "Ctrl";
		case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
		case GLFW_KEY_LEFT_ALT: return "Alt";
		case GLFW_KEY_RIGHT_ALT: return "Right Alt";
		case GLFW_KEY_UP: return "Up";
		case GLFW_KEY_DOWN: return "Down";
		case GLFW_KEY_LEFT: return "Left";
		case GLFW_KEY_RIGHT: return "Right";
		case GLFW_KEY_F1: return "F1";
		case GLFW_KEY_F2: return "F2";
		case GLFW_KEY_F3: return "F3";
		case GLFW_KEY_F4: return "F4";
		case GLFW_KEY_F5: return "F5";
		case GLFW_KEY_F6: return "F6";
		case GLFW_KEY_F7: return "F7";
		case GLFW_KEY_F8: return "F8";
		case GLFW_KEY_F9: return "F9";
		case GLFW_KEY_F10: return "F10";
		case GLFW_KEY_F11: return "F11";
		case GLFW_KEY_F12: return "F12";
		default: break;
	}

	const char* name = glfwGetKeyName(glfw_key, 0);
	if (name)
		return name;

	return "Key " + std::to_string(glfw_key);
}

} // namespace ve