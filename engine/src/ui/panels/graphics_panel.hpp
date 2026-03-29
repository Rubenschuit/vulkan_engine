#pragma once
#include "ui/editor_panel.hpp"

namespace ve {

class VeRenderer;
class EventBus;

class VENGINE_API GraphicsPanel : public EditorPanel {
public:
	GraphicsPanel(VeRenderer& renderer, EventBus& event_bus)
		: m_renderer(renderer), m_event_bus(event_bus) {}

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Graphics"; }

private:
	VeRenderer& m_renderer;
	EventBus& m_event_bus;
};

} // namespace ve
