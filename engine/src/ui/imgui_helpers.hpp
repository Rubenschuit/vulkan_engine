#pragma once

// Shared Dear ImGui layout helpers + semantic colors for the editor panels.
// Pairs with editor_icons.hpp; both are leaf includes pulled in by panels.

#include <imgui.h>
#include <cfloat>
#include <utility>

namespace ve::ui {

// Standard label-column width for two-column rows. Sized for short property
// names (inspector/environment); long descriptive setting labels (e.g. the
// graphics/debug panels) should stay single-line to avoid truncation.
inline constexpr float LABEL_COLUMN_W = 110.0f;

// Semantic colors, centralizing scattered ImVec4 literals across panels.
inline const ImVec4 COL_WARNING{1.0f, 0.8f, 0.2f, 1.0f};
inline const ImVec4 COL_ERROR{1.0f, 0.4f, 0.4f, 1.0f};
inline const ImVec4 COL_MUTED{0.7f, 0.7f, 0.7f, 1.0f};
inline const ImVec4 COL_AXIS_X{0.7f, 0.15f, 0.15f, 1.0f};
inline const ImVec4 COL_AXIS_Y{0.15f, 0.6f, 0.15f, 1.0f};
inline const ImVec4 COL_AXIS_Z{0.15f, 0.15f, 0.7f, 1.0f};
inline const ImVec4 COL_AXIS_X_HOVER{0.85f, 0.2f, 0.2f, 1.0f};
inline const ImVec4 COL_AXIS_Y_HOVER{0.2f, 0.75f, 0.2f, 1.0f};
inline const ImVec4 COL_AXIS_Z_HOVER{0.2f, 0.2f, 0.85f, 1.0f};

// Two-column labeled row: left = fixed-width label, right = full-width widget.
// When a resetFn is supplied, right-clicking the value opens a "Reset to default"
// context menu, keeping the label column free of buttons.
template <typename WidgetFn, typename... ResetFn>
void labeledWidget(float label_w, const char* text, WidgetFn widgetFn, ResetFn... resetFn) {
	ImGui::Columns(2, nullptr, false);
	ImGui::SetColumnWidth(0, label_w);
	ImGui::AlignTextToFramePadding();
	ImGui::Text("%s", text);
	ImGui::NextColumn();
	ImGui::SetNextItemWidth(-FLT_MIN);
	widgetFn();
	if constexpr (sizeof...(resetFn) > 0) {
		ImGui::PushID(text);
		if (ImGui::BeginPopupContextItem("reset", ImGuiPopupFlags_MouseButtonRight)) {
			if (ImGui::MenuItem("Reset to default"))
				(resetFn(), ...);
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	ImGui::Columns(1);
}

// Same, using the standard label-column width.
template <typename WidgetFn, typename... ResetFn>
void labeledWidget(const char* text, WidgetFn widgetFn, ResetFn... resetFn) {
	labeledWidget(LABEL_COLUMN_W, text, std::move(widgetFn), std::move(resetFn)...);
}

// Scalar slider row with right-click reset-to-default; ORs edits into `changed`.
inline void sliderRow(float label_w, const char* text, const char* id, float* v,
		float lo, float hi, float def, bool& changed) {
	labeledWidget(label_w, text, [&]() {
		if (ImGui::SliderFloat(id, v, lo, hi))
			changed = true;
	}, [&]() { *v = def; changed = true; });
}

inline void dragRow(float label_w, const char* text, const char* id, float* v,
		float speed, float lo, float hi, const char* fmt, float def, bool& changed) {
	labeledWidget(label_w, text, [&]() {
		if (ImGui::DragFloat(id, v, speed, lo, hi, fmt))
			changed = true;
	}, [&]() { *v = def; changed = true; });
}

} // namespace ve::ui
