#pragma once

// Defines action bindings for the data-driven input system.
// Actions are registered on InputController and dispatched via EventBus as InputActionEvents.

#include "ve_export.hpp"
#include <string>

namespace ve {

enum class InputContext : uint8_t {
	GameMode,
	EditorMode,
	Always
};

enum class TriggerType : uint8_t {
	OnPress,
	OnHold
};

struct ActionBinding {
	std::string name;
	int key = 0;
	TriggerType trigger = TriggerType::OnPress;
	InputContext context = InputContext::GameMode;
	std::string description;
	uint32_t value = 0;
};

struct InputActions {
	float move_forward = 0.0f;
	float move_right = 0.0f;
	float move_up = 0.0f;
	float look_yaw = 0.0f;
	float look_pitch = 0.0f;
	float mouse_dx = 0.0f;
	float mouse_dy = 0.0f;
	bool  sprint = false;
	bool  jump = false;
	bool  mouse_look_enabled = true;
};

VENGINE_API std::string keyDisplayName(int glfw_key);

} // namespace ve
