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
	EVIDENCE = 4, // written into the sky-visibility atlas
	PROBE_J = 11,
	COVERAGE = 12,
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
	case RcDebugView::COVERAGE:
	case RcDebugView::TRACE_KIND:
	case RcDebugView::CONVERGENCE:
	case RcDebugView::PROBE_GRID:
	case RcDebugView::SPEC:
		return true;
	}
	return false;
}

struct RcSettings {
	bool enabled = true;
	float thickness = 1.0f;
	float intensity = 1.0f;
	float short_range_ao = 1.0f; // how much GTAO applies to the RC share
	float spec_occlusion = 1.0f;
	float luma_clamp = 20.0f;
	int resolution_div = 4;
	int rays_per_pixel = 1;
	float ds0 = 0.075f;      // c0 probe spacing at LOD 0
	// c0 polar bins: directions per c0 probe = 2 * c0_polar_bins^2 (32 at 4),
	// four times more per cascade out
	int c0_polar_bins = 4;
	// c0 ray length as a multiple of the probe spacing; each cascade out
	// reaches four times further
	float c0_ray_length = 1.6f;
	float lod0_dist = 3.0f;
	// Minimum fraction of a ray's full length that must be
	// confirmed empty before a screen-edge sky exit's continuation is trusted
	// as a full-length clear (0 = any confident exit, 1 = never)
	float sky_min_frac = 0.0625f;
	// How much the probes' sky estimate replaces the environment: 0 = the
	// environment (or flat ambient) everywhere, bounce only from the probes;
	// 1 = the probes' estimate; between, a linear blend
	float sky_visibility = 1.0f;
	// Validate the bounce-history reprojection against last frame's depth
	bool hist_depth_check = true;
	// Rough specular from the probe store: the gather taps the irradiance tile
	// at the reflection direction (a cosine lobe around it) and the composite
	// lerps the cubemap toward it above spec_handoff_roughness
	bool rough_specular = true;
	// Roughness at which the probes' rough specular fully replaces the
	// prefiltered cubemap
	float spec_handoff_roughness = 0.4f;
	int probe_ttl = 16; // frames a probe missing from insertion stays alive
	float blend_zone = 0.35f;
	// Parent-insertion: 0 = all 8 corners, the set the merge tap reads,
	// 1 = single nearest parent
	int parent_mode = 1;
	// Which c0 probes a pixel inserts and its rays deposit into
	int deposit_mode = 0;
	// The merge weights each parent by its slot's sample count (frames with
	// samples, up to 63) on top of the trilinear weight; off = position only.
	// Parents with no samples are skipped either way. Read side only
	bool count_weighted_read = true;
	int cascades = 4;
	int memory_mb = 448;
	bool freeze_probes = false;
	bool single_frame = false;
	int reset_requests = 0;
	RcDebugView debug_view = RcDebugView::NONE;
	int probe_j_cascade = 0; // cascade shown in PROBE_J / PROBE_GRID views
};

}
