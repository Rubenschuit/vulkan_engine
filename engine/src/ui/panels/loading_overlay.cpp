#include "pch.hpp"
#include "ui/panels/loading_overlay.hpp"
#include "resources/asset_loading_system.hpp"

#include <imgui.h>

namespace ve {

void LoadingPanel::render(AssetLoadingSystem& loader) {
	if (loader.getState() == LoadState::IDLE || loader.getState() == LoadState::READY)
		return;

	ImGuiIO& io = ImGui::GetIO();
	ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(400, 0));
	ImGui::Begin("Loading", nullptr,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_AlwaysAutoResize);

	std::string title = "Loading " + loader.getModelName() + "...";
	float title_width = ImGui::CalcTextSize(title.c_str()).x;
	ImGui::SetCursorPosX((400 - title_width) * 0.5f);
	ImGui::Text("%s", title.c_str());

	ImGui::Spacing();

	float progress = loader.getProgress();
	if (progress < 0.f) {
		float t = static_cast<float>(ImGui::GetTime());
		float pulse = (std::sin(t * 3.f) + 1.f) * 0.5f;
		ImGui::ProgressBar(pulse, ImVec2(-1, 0), "");
	} else {
		ImGui::ProgressBar(progress, ImVec2(-1, 0),
			(std::to_string(static_cast<int>(progress * 100.f)) + "%").c_str());
	}

	ImGui::Spacing();

	std::string status = loader.getStatusMessage();
	if (!status.empty()) {
		float status_width = ImGui::CalcTextSize(status.c_str()).x;
		float avail = ImGui::GetContentRegionAvail().x;
		ImGui::SetCursorPosX((avail - status_width) * 0.5f);
		ImGui::TextDisabled("%s", status.c_str());
	}

	ImGui::Spacing();

	float button_width = 80.f;
	ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - button_width) * 0.5f);
	if (ImGui::Button("Cancel", ImVec2(button_width, 0)))
		loader.cancel();

	ImGui::End();
}

} // namespace ve
