#include "pch.hpp"
#include "ui/panels/loading_overlay.hpp"
#include "resources/asset_loading_system.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace ve {

namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr float TOAST_WIDTH = 320.f;
constexpr float TOAST_MARGIN = 16.f;
constexpr float BAR_HEIGHT = 8.f;
constexpr float SPINNER_RADIUS = 8.f;
constexpr float SPINNER_THICKNESS = 2.f;
constexpr ImU32 BAR_BG_COLOR = IM_COL32(255, 255, 255, 30);

inline ImU32 accentColor() {
	return ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
}

void drawSpinner(ImDrawList* dl, ImVec2 center, float radius, float thickness, float t) {
	const int segments = 32;
	const float arc_len = PI * 1.5f;
	const float start = std::fmod(t * 2.f, PI * 2.f);
	dl->PathClear();
	for (int i = 0; i <= segments; ++i) {
		float a = start + arc_len * (static_cast<float>(i) / segments);
		dl->PathLineTo(ImVec2(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius));
	}
	dl->PathStroke(accentColor(), ImDrawFlags_None, thickness);
}

void drawIndeterminateBar(ImDrawList* dl, ImVec2 min, ImVec2 max, float t) {
	const float rounding = (max.y - min.y) * 0.5f;
	dl->AddRectFilled(min, max, BAR_BG_COLOR, rounding);
	const float width = max.x - min.x;
	const float band_w = width * 0.28f;
	// phase ramps 0..1 then snaps back; band slides off-screen on each side
	const float phase = std::fmod(static_cast<float>(t) * 0.9f, 1.0f);
	const float band_x0 = min.x + phase * (width + band_w) - band_w;
	const float band_x1 = band_x0 + band_w;
	const ImVec2 clipped_min(std::max(band_x0, min.x), min.y);
	const ImVec2 clipped_max(std::min(band_x1, max.x), max.y);
	if (clipped_max.x > clipped_min.x)
		dl->AddRectFilled(clipped_min, clipped_max, accentColor(), rounding);
}

void drawProgressBar(ImDrawList* dl, ImVec2 min, ImVec2 max, float fraction) {
	const float rounding = (max.y - min.y) * 0.5f;
	dl->AddRectFilled(min, max, BAR_BG_COLOR, rounding);
	fraction = std::clamp(fraction, 0.f, 1.f);
	const float fill_w = (max.x - min.x) * fraction;
	if (fill_w > 0.5f) {
		ImVec2 fill_max(min.x + fill_w, max.y);
		dl->AddRectFilled(min, fill_max, accentColor(), rounding);
	}
}

std::string truncateToWidth(const std::string& s, float max_width) {
	if (ImGui::CalcTextSize(s.c_str()).x <= max_width)
		return s;
	const char* ellipsis = "...";
	const float ellipsis_w = ImGui::CalcTextSize(ellipsis).x;
	if (max_width <= ellipsis_w)
		return ellipsis;
	size_t lo = 0;
	size_t hi = s.size();
	while (lo < hi) {
		size_t mid = (lo + hi + 1) / 2;
		std::string candidate = s.substr(0, mid) + ellipsis;
		if (ImGui::CalcTextSize(candidate.c_str()).x <= max_width)
			lo = mid;
		else
			hi = mid - 1;
	}
	return s.substr(0, lo) + ellipsis;
}

} // namespace

void LoadingPanel::render(AssetLoadingSystem& loader, ImVec2 viewport_min, ImVec2 viewport_max) {
	const LoadState ls = loader.getState();
	if (ls == LoadState::IDLE || ls == LoadState::READY) {
		m_was_active = false;
		m_displayed_progress = 0.f;
		return;
	}

	const float now = static_cast<float>(ImGui::GetTime());
	const float dt = m_was_active ? std::max(0.f, now - m_last_time) : 0.f;
	m_last_time = now;
	if (!m_was_active)
		m_displayed_progress = 0.f;
	m_was_active = true;

	// Anchor: viewport bottom-right with margin, fallback to OS-window
	ImVec2 anchor;
	const bool have_viewport = (viewport_max.x > viewport_min.x) && (viewport_max.y > viewport_min.y);
	if (have_viewport) {
		anchor = ImVec2(viewport_max.x - TOAST_MARGIN, viewport_max.y - TOAST_MARGIN);
	} else {
		ImVec2 disp = ImGui::GetIO().DisplaySize;
		anchor = ImVec2(disp.x - TOAST_MARGIN, disp.y - TOAST_MARGIN);
	}

	ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(TOAST_WIDTH, 0.f));

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 12.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 8.f));
	ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
	bg.w = 0.92f;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings;

	ImGui::Begin("##LoadingToast", nullptr, flags);

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Row 1: spinner + title
	{
		ImVec2 row_pos = ImGui::GetCursorScreenPos();
		const float line_h = ImGui::GetTextLineHeight();
		const float spinner_box = SPINNER_RADIUS * 2.f + 2.f;
		ImVec2 spinner_center(row_pos.x + SPINNER_RADIUS, row_pos.y + line_h * 0.5f);
		drawSpinner(dl, spinner_center, SPINNER_RADIUS, SPINNER_THICKNESS, now);

		const float title_x = spinner_box + 8.f;
		ImGui::Dummy(ImVec2(title_x, line_h));
		ImGui::SameLine();
		const float avail = ImGui::GetContentRegionAvail().x;
		const std::string title = truncateToWidth(loader.getModelName(), avail);
		ImGui::TextUnformatted(title.c_str());
	}

	// Row 2: progress bar + (optional) % label
	{
		const float target = loader.getProgress();
		const bool indeterminate = target < 0.f;
		if (!indeterminate) {
			const float k = std::min(1.f, dt * 8.f);
			m_displayed_progress += (target - m_displayed_progress) * k;
		}

		const float pct_w = indeterminate ? 0.f : ImGui::CalcTextSize("100%").x;
		const float gap = indeterminate ? 0.f : 8.f;
		const float total_w = ImGui::GetContentRegionAvail().x;
		const float bar_w = std::max(20.f, total_w - pct_w - gap);

		ImVec2 bar_min = ImGui::GetCursorScreenPos();
		// Vertically center the bar in a row of line-height
		const float row_h = ImGui::GetTextLineHeight();
		bar_min.y += (row_h - BAR_HEIGHT) * 0.5f;
		ImVec2 bar_max(bar_min.x + bar_w, bar_min.y + BAR_HEIGHT);

		if (indeterminate)
			drawIndeterminateBar(dl, bar_min, bar_max, now);
		else
			drawProgressBar(dl, bar_min, bar_max, m_displayed_progress);

		ImGui::Dummy(ImVec2(bar_w, row_h));
		if (!indeterminate) {
			ImGui::SameLine(0.f, gap);
			ImGui::TextDisabled("%d%%", static_cast<int>(m_displayed_progress * 100.f + 0.5f));
		}
	}

	// Row 3: status + (optional) cancel
	{
		const std::string status = loader.getStatusMessage();
		const bool show_cancel = (ls != LoadState::FINALIZING);

		float cancel_w = 0.f;
		if (show_cancel) {
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f, 2.f));
			cancel_w = ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2.f;
			ImGui::PopStyleVar();
		}

		const float avail = ImGui::GetContentRegionAvail().x;
		const float status_max_w = std::max(0.f, avail - cancel_w - 8.f);
		const std::string status_truncated = truncateToWidth(status, status_max_w);

		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", status_truncated.c_str());

		if (show_cancel) {
			ImGui::SameLine(avail - cancel_w + ImGui::GetStyle().WindowPadding.x);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f, 2.f));
			if (ImGui::SmallButton("Cancel"))
				loader.cancel();
			ImGui::PopStyleVar();
		}
	}

	ImGui::End();

	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}

} // namespace ve