#pragma once
#include "ui/editor_panel.hpp"

namespace ve {

class VeRenderer;

class VENGINE_API GraphicsPanel : public EditorPanel {
public:
	explicit GraphicsPanel(VeRenderer& renderer) : m_renderer(renderer) {}

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Graphics"; }

private:
	VeRenderer& m_renderer;
};

} // namespace ve
