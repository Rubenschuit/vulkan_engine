#pragma once
#include "ui/editor_panel.hpp"

namespace ve {

class EditorCameraController;
class ShadowRenderSystem;
class TextureInspector;

class VENGINE_API DebugPanel : public EditorPanel {
public:
	explicit DebugPanel(TextureInspector& texture_inspector)
		: m_texture_inspector(texture_inspector) {}

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Debug"; }

	void setShadowRenderSystem(ShadowRenderSystem* system) { m_shadow_render_system = system; }
	void setEditorCamera(const EditorCameraController* camera) { m_editor_camera = camera; }

private:
	TextureInspector& m_texture_inspector;
	ShadowRenderSystem* m_shadow_render_system = nullptr;
	const EditorCameraController* m_editor_camera = nullptr;
};

} // namespace ve
