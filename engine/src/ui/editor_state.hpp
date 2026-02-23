#pragma once
#include "ve_export.hpp"
#include "scene/ve_entity.hpp"

namespace ve {

struct VENGINE_API EditorState {
	// Mode
	bool editor_mode = true; // true = docking editor, false = fullscreen

	// Selection
	Entity selected_entity = Entity::null();
	bool selection_changed = false;

	// Panel visibility
	bool show_hierarchy = true;
	bool show_inspector = true;
	bool show_viewport = true;
	bool show_performance = true;
	bool show_settings = true;
	bool show_environment = true;

	// Viewport state (updated by viewport panel each frame)
	bool viewport_hovered = false;
	bool viewport_focused = false;
	float viewport_width = 0.0f;
	float viewport_height = 0.0f;
};

} // namespace ve
