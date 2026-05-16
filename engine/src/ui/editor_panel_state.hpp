#pragma once
#include "ve_export.hpp"

namespace ve {

struct VENGINE_API EditorPanelState {
	bool visible = false;          // editor-mode toggle
	bool show_controls = true;     // controls overlay ui when fullscreen
};

} // namespace ve