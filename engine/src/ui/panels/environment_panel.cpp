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

	// --- IBL ---
	ImGui::Text("Image-Based Lighting");
	ImGui::Separator();
	ImGui::Checkbox("Enabled##ibl", &ctx.ibl_enabled);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Use skybox environment for ambient lighting (requires IBL data).");
	if (ctx.ibl_enabled) {
		ImGui::Checkbox("Auto Exposure##ibl", &ctx.ibl_auto_exposure);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Automatically compensate intensity for dark environments.");
		if (ctx.ibl_auto_exposure) {
			ImGui::SameLine();
			ImGui::TextDisabled("(%.1fx)", ctx.ibl_exposure_compensation);
		}
		ImGui::SliderFloat("Diffuse##ibl", &ctx.ibl_diffuse_intensity, 0.0f, 5.0f, "%.2f");
		if (ImGui::IsItemHovered()) {
			float effective = ctx.ibl_diffuse_intensity * (ctx.ibl_auto_exposure ? ctx.ibl_exposure_compensation : 1.0f);
			ImGui::SetTooltip("Diffuse IBL contribution. Effective: %.2f", effective);
		}
		ImGui::SliderFloat("Specular##ibl", &ctx.ibl_specular_intensity, 0.0f, 5.0f, "%.2f");
		if (ImGui::IsItemHovered()) {
			float effective = ctx.ibl_specular_intensity * (ctx.ibl_auto_exposure ? ctx.ibl_exposure_compensation : 1.0f);
			ImGui::SetTooltip("Specular IBL contribution. Effective: %.2f", effective);
		}
		ImGui::SliderFloat("Min Ambient##ibl", &ctx.ibl_min_ambient, 0.0f, 0.05f, "%.4f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Minimum ambient floor to prevent completely black areas.");
		if (m_skybox) {
			const auto& available = m_skybox->getAvailableSkyboxes();
			size_t idx = m_skybox->getCurrentSkyboxIndex();
			if (idx < available.size() && !available[idx].has_ibl)
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No IBL data for this skybox");
		}
	}

	ImGui::Spacing();

	// --- Ambient Light ---
	ImGui::Text("Ambient Light");
	ImGui::Separator();
	ImGui::ColorEdit3("Color##ambient", &ctx.ambient_light_color.r);
	ImGui::SliderFloat("Intensity##ambient", &ctx.ambient_light_intensity, 0.0f, 0.5f, "%.4f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Flat ambient (used when IBL is off or unavailable).");

	ImGui::End();
}

} // namespace ve
