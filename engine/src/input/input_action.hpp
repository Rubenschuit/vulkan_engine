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

VENGINE_API std::string keyDisplayName(int glfw_key);

} // namespace ve
