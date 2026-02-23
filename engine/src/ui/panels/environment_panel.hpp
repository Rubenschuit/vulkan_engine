#pragma once
#include "ui/editor_panel.hpp"

namespace ve {

class SkyboxRenderSystem;

class VENGINE_API EnvironmentPanel : public EditorPanel {
public:
	explicit EnvironmentPanel(SkyboxRenderSystem* skybox = nullptr) : m_skybox(skybox) {}

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Environment"; }

	void setSkyboxSystem(SkyboxRenderSystem* skybox) { m_skybox = skybox; }

private:
	SkyboxRenderSystem* m_skybox = nullptr;
};

} // namespace ve
