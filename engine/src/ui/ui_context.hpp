#pragma once
#include "ve_export.hpp"
#include "rendering/render_settings.hpp"
#include "rendering/frame_stats.hpp"
#include "application/simulation_settings.hpp"

namespace ve {

// Bundle of references passed to UI rendering
struct VENGINE_API UIContext {
	RenderSettings& settings;
	const FrameStats& stats;
	SimulationSettings& sim;
};

}