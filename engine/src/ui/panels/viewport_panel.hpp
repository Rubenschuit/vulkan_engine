#pragma once
#include "ui/editor_panel.hpp"
#include "ui/editor_state.hpp"
#include <imgui.h>
#include <vulkan/vulkan.h>

namespace ve {

class VENGINE_API ViewportPanel : public EditorPanel {
public:
	void setTextureID(VkDescriptorSet texture_id) { m_texture_id = texture_id; }

	void render(Registry* /*registry*/, EditorState& state, UIContext& /*context*/) override {
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		if (ImGui::Begin("Viewport", &state.show_viewport)) {
			state.viewport_hovered = ImGui::IsWindowHovered();
			state.viewport_focused = ImGui::IsWindowFocused();

			ImVec2 size = ImGui::GetContentRegionAvail();
			float dpi_scale = ImGui::GetIO().DisplayFramebufferScale.x;
			state.viewport_width = size.x * dpi_scale;
			state.viewport_height = size.y * dpi_scale;

			if (m_texture_id != VK_NULL_HANDLE && size.x > 0 && size.y > 0)
				ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(m_texture_id)), size);
			else
				ImGui::Text("No viewport image");
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

	const char* getName() const override { return "Viewport"; }

private:
	VkDescriptorSet m_texture_id = VK_NULL_HANDLE;
};

} // namespace ve
