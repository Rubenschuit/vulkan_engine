#pragma once
#include "ui/editor_panel.hpp"
#include "ui/editor_state.hpp"
#include <vulkan/vulkan.h>

namespace ve {

class VeCamera;

class VENGINE_API ViewportPanel : public EditorPanel {
public:
	void setTextureID(VkDescriptorSet texture_id) { m_texture_id = texture_id; }
	void setCamera(VeCamera* camera) { m_camera = camera; }

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Viewport"; }

private:
	void renderGizmoToolbar(EditorState& state);
	void renderGizmo(Registry* registry, EditorState& state, float img_x, float img_y, float img_w, float img_h);

	VkDescriptorSet m_texture_id = VK_NULL_HANDLE;
	VeCamera* m_camera = nullptr;
};

} // namespace ve
