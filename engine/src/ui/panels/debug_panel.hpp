#pragma once
#include "ui/editor_panel.hpp"

namespace ve {

class FlyCameraController;
class ShadowRenderSystem;
class TextureInspector;

class VENGINE_API DebugPanel : public EditorPanel {
public:
	explicit DebugPanel(TextureInspector& texture_inspector)
		: m_texture_inspector(texture_inspector) {}

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Debug"; }

	void setShadowRenderSystem(ShadowRenderSystem* system) { m_shadow_render_system = system; }
	void setFlyCamera(const FlyCameraController* camera) { m_fly_camera = camera; }

private:
	TextureInspector& m_texture_inspector;
	ShadowRenderSystem* m_shadow_render_system = nullptr;
	const FlyCameraController* m_fly_camera = nullptr;
};

} // namespace ve
