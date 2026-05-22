#pragma once
#include "ve_export.hpp"
#include <imgui.h>

namespace ve {

class AssetLoadingSystem;

class VENGINE_API LoadingPanel {
public:
	void render(AssetLoadingSystem& loader,
	            ImVec2 viewport_min = ImVec2(0.f, 0.f),
	            ImVec2 viewport_max = ImVec2(0.f, 0.f));

private:
	float m_displayed_progress = 0.f;
	float m_last_time = 0.f;
	bool m_was_active = false;
};

}
