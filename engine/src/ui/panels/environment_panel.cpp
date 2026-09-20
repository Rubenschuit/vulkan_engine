#include "pch.hpp"
#include "ui/panels/environment_panel.hpp"
#include "ui/editor_state.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/imgui_helpers.hpp"
#include "rendering/skybox_render_system.hpp"
#include <imgui.h>

namespace ve {

using namespace ve::ui;

void EnvironmentPanel::render(Registry* /*registry*/, EditorState& state, UIContext& ctx) {
	if (!ImGui::Begin("Environment", &state.show_environment, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	ImGui::SeparatorText("Skybox");
	if (m_skybox) {
		auto& settings = m_skybox->getSettings();
		if (m_skybox->isLoading())
			ImGui::TextColored(COL_WARNING, "Loading...");
		const auto& available = m_skybox->getAvailableSkyboxes();
		if (!available.empty()) {
			int current_idx = static_cast<int>(m_skybox->getCurrentSkyboxIndex());
			for (size_t i = 0; i < available.size(); i++)
				if (ImGui::RadioButton(available[i].display_name.c_str(), &current_idx, static_cast<int>(i)))
					m_skybox->setSkybox(i);
		} else {
			ImGui::TextDisabled("No skybox textures found (.ktx, .ktx2)");
		}
		labeledWidget("Rotate", [&]() {
			ImGui::Checkbox("##rotate", &settings.rotate);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rotates the drawn sky box only, not the lighting.");
		});
		labeledWidget("Exposure", [&]() {
			ImGui::SliderFloat("##skybox_exposure", &settings.exposure, 0.1f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Brightness of the sky as drawn and, with IBL enabled, as it lights the scene.");
		});
		labeledWidget("Day", [&]() {
			ImGui::Checkbox("##day", &settings.is_day);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Day: warm tint, Night: cool tint.");
		});
	} else {
		ImGui::TextDisabled("Skybox system not available");
	}

	ImGui::SeparatorText("Image-Based Lighting");
	labeledWidget("Enabled", [&]() {
		ImGui::Checkbox("##ibl_enabled", &ctx.settings.ibl_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Use skybox environment for diffuse and specular ambient lighting\n(requires IBL data). SSR and RC Rough Specular only apply while on.");
	});
	if (ctx.settings.ibl_enabled) {
		labeledWidget("Min Ambient", [&]() {
			ImGui::SliderFloat("##ibl_min_ambient", &ctx.settings.ibl_min_ambient, 0.0f, 0.05f, "%.4f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Minimum ambient floor, scaled by albedo.");
		});
		if (m_skybox) {
			const auto& available = m_skybox->getAvailableSkyboxes();
			size_t idx = m_skybox->getCurrentSkyboxIndex();
			if (idx < available.size() && !available[idx].has_ibl)
				ImGui::TextColored(COL_WARNING, "No IBL data for this skybox");
		}
	}

	ImGui::SeparatorText("Ambient Light");
	labeledWidget("Color", [&]() {
		ImGui::ColorEdit3("##ambient_color", &ctx.settings.ambient_light_color.r);
	});
	labeledWidget("Intensity", [&]() {
		ImGui::SliderFloat("##ambient_intensity", &ctx.settings.ambient_light_intensity, 0.0f, 0.5f, "%.4f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Flat ambient (used when IBL is off or unavailable).");
	});

	ImGui::End();
}

} // namespace ve
