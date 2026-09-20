#pragma once
#include "ve_export.hpp"

#include <cstdint>

namespace ve {

// Renderer writes, UI reads.
struct VENGINE_API FrameStats {
	// Frame-level CPU/GPU timings (ms)
	float cpu_time = 0.0f;
	float fence_wait = 0.0f;
	float acquire_wait = 0.0f;
	float gpu_time = 0.0f;
	float compute_gpu_time = 0.0f;

	// Per-system GPU breakdown (ms)
	float gpu_culling = 0.0f;
	float gpu_shadow_maps = 0.0f;
	float gpu_geometry_prepass = 0.0f;
	float gpu_gtao = 0.0f;
	float gpu_scene_render = 0.0f;
	float gpu_ssr = 0.0f;
	float gpu_rc = 0.0f;
	float gpu_rc_build = 0.0f;
	float gpu_rc_shade = 0.0f;
	float gpu_rc_map = 0.0f;
	float gpu_rc_trace = 0.0f;
	float gpu_rc_resolve = 0.0f;
	float gpu_rc_merge = 0.0f;
	float gpu_rc_irradiance = 0.0f;
	float gpu_rc_gather = 0.0f;
	float gpu_bloom = 0.0f;
	float gpu_post_process = 0.0f;
	float gpu_hiz = 0.0f;
	float gpu_shadow_mask = 0.0f;
	float gpu_outline = 0.0f;
	float gpu_skinning = 0.0f;
	float gpu_cluster_lights = 0.0f;
	float gpu_particles = 0.0f;

	// Per-system CPU breakdown (ms)
	float cpu_culling = 0.0f;
	float cpu_shadow_maps = 0.0f;
	float cpu_geometry_prepass = 0.0f;
	float cpu_gtao = 0.0f;
	float cpu_scene_render = 0.0f;
	float cpu_ssr = 0.0f;
	float cpu_rc = 0.0f;
	float cpu_bloom = 0.0f;
	float cpu_post_process = 0.0f;
	float cpu_hiz = 0.0f;
	float cpu_shadow_mask = 0.0f;
	float cpu_outline = 0.0f;
	float cpu_physics = 0.0f;
	float cpu_ui = 0.0f;
	float cpu_skinning = 0.0f;
	float cpu_cluster_lights = 0.0f;
	float cpu_particles = 0.0f;

	// Culling / scene counters
	uint32_t cull_total_objects = 0;
	uint32_t cull_visible_objects = 0;
	uint32_t visible_triangles = 0;
	uint32_t visible_meshlets = 0;
	uint32_t num_point_lights = 0;
	uint32_t num_directional_lights = 0;
	uint32_t num_spot_lights = 0;
	uint32_t num_area_lights = 0;

	// Radiance Cascades probe store
	uint32_t rc_probes = 0;
	uint32_t rc_overflow = 0;
	uint32_t rc_probes_c0 = 0;
	uint32_t rc_probes_c1 = 0;
	uint32_t rc_probes_c2 = 0;
	uint32_t rc_probes_c3 = 0;
	uint32_t rc_probes_c4 = 0;
	uint32_t rc_probes_c5 = 0;
	float rc_rays_hit = 0.0f;
	float rc_rays_clear = 0.0f;
	float rc_rays_sky = 0.0f;
	float rc_rays_sky_gated = 0.0f;
	float rc_rays_unk_occluded = 0.0f;
	float rc_rays_unk_stepcap = 0.0f;
	float rc_rays_unk_edge = 0.0f;
	float rc_rays_unk_clip = 0.0f;
};

}