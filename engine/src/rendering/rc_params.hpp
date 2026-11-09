/*
 * RC (Radiance Cascades GI) user settings + debug params, shared by
 * RenderSettings and RcSystem.
 */

#pragma once
#include <cstdint>

namespace ve {

// One RC view at a time; it overrides the render mode while set. 1-4 display
// a gather output as it is; 10 and above replace the irradiance output with
// the view and must match RCW_GATHER_* in rc_world_common.slangh.
enum class RcDebugView : uint32_t {
	NONE = 0,
	IRRADIANCE = 1,
	SKY_VISIBILITY = 2,
	UPSAMPLE_MATCH = 3,
	EVIDENCE = 4,
	PROBE_J = 11,
	TRACE_KIND = 13,
	CONVERGENCE = 14,
	PROBE_GRID = 15,
	SPEC = 19,
};

constexpr bool isRcDebugView(uint32_t id) {
	switch (static_cast<RcDebugView>(id)) {
	case RcDebugView::NONE:
	case RcDebugView::IRRADIANCE:
	case RcDebugView::SKY_VISIBILITY:
	case RcDebugView::UPSAMPLE_MATCH:
	case RcDebugView::EVIDENCE:
	case RcDebugView::PROBE_J:
	case RcDebugView::TRACE_KIND:
	case RcDebugView::CONVERGENCE:
	case RcDebugView::PROBE_GRID:
	case RcDebugView::SPEC:
		return true;
	}
	return false;
}

// How the probes' sky estimate meets the environment (IBL, or the flat
// ambient), ordered from the most sky kept to the least. AUTO is
// PROBE_ESTIMATE under ray query and OFF under the screen-space tracer, whose
// unresolved rays leave the estimate too dark wherever the sky is off screen.
enum class RcSkyOcclusion : uint32_t {
	AUTO = 0,
	OFF = 1,                // the environment as with GI off; RC adds bounce only
	UNKNOWN_SKY = 2,        // directions no ray resolved count as sky
	PROBE_ESTIMATE = 3,     // they take the probe's proven-sky share
	UNKNOWN_OBSTRUCTED = 4, // they count as blocked
};

constexpr int RC_MAX_HISTORY_LENGTH = 64;

struct RcSettings {
	bool enabled = true;
	float luma_clamp = 20.0f;
	int resolution_div = 4;
	int rays_per_pixel = 1;
	int history_length = 64; // sampled frames a bin averages before its EMA weight floors at 1 / history_length
	int probe_ttl = 16;      // frames a probe missing from insertion stays alive
	float ds0 = 0.075f;      // c0 probe spacing at LOD 0
	float lod0_dist = 3.0f;
	bool ray_query = true; // trace the acceleration structures, where the device has ray query
	bool trace_deformed = true; // skinned / morphed meshes in those structures, refitted every frame
	RcSkyOcclusion sky_occlusion = RcSkyOcclusion::AUTO;
	bool specular = true;
	int cascades = 4;
	int memory_mb = 578;     // the whole probe store
	bool single_frame = false;
	RcDebugView debug_view = RcDebugView::NONE;
	int probe_j_cascade = 0; // cascade shown in PROBE_J / PROBE_GRID views
};

}
