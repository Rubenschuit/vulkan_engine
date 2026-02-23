#pragma once
#include "ve_export.hpp"

namespace ve {

class Registry;
struct EditorState;
struct UIContext;

class VENGINE_API EditorPanel {
public:
	virtual ~EditorPanel() = default;
	virtual void render(Registry* registry, EditorState& state, UIContext& context) = 0;
	virtual const char* getName() const = 0;
};

} // namespace ve
