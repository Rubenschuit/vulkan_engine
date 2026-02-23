#include "pch.hpp"
#include "ui/panels/environment_panel.hpp"
#include "ui/editor_state.hpp"
#include "ui/imgui_layer.hpp"
#include "rendering/skybox_render_system.hpp"
#include <imgui.h>

namespace ve {

void EnvironmentPanel::render(Registry* /*registry*/, EditorState& state, UIContext& ctx) {
	if (!ImGui::Begin("Environment", &state.show_environment, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	// --- Skybox ---
	ImGui::Text("Skybox");
	ImGui::Separator();
	if (m_skybox) {
		auto& settings = m_skybox->getSettings();
		if (m_skybox->isLoading())
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Loading...");
		const auto& available = m_skybox->getAvailableSkyboxes();
		if (!available.empty()) {
			int current_idx = static_cast<int>(m_skybox->getCurrentSkyboxIndex());
			for (size_t i = 0; i < available.size(); i++)
				if (ImGui::RadioButton(available[i].display_name.c_str(), &current_idx, static_cast<int>(i)))
					m_skybox->setSkybox(i);
		} else {
			ImGui::TextDisabled("No skybox textures found (.ktx, .ktx2)");
		}
		ImGui::Checkbox("Rotate", &settings.rotate);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Disable for skyboxes with a horizon (e.g. clouds).");
		ImGui::SliderFloat("Exposure##skybox", &settings.exposure, 0.1f, 5.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Skybox brightness (independent of post-process exposure).");
		ImGui::Checkbox("Day", &settings.is_day);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Day: warm tint, Night: cool tint.");
	} else {
		ImGui::TextDisabled("Skybox system not available");
	}

	ImGui::Spacing();

	// --- Ambient Light ---
	ImGui::Text("Ambient Light");
	ImGui::Separator();
	ImGui::ColorEdit3("Color##ambient", &ctx.ambient_light_color.r);
	ImGui::SliderFloat("Intensity##ambient", &ctx.ambient_light_intensity, 0.0f, 0.5f, "%.4f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Global ambient light intensity applied to all surfaces.");

	ImGui::End();
}

} // namespace ve
