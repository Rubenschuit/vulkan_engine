// Trace + deposit. One thread per spawn pixel, push.rays_per_pixel rays over
// the hemisphere around its normal, each split into per-cascade interval
// deposits
// rcw_trace_rayquery_comp includes
// this file to trace the acceleration structures where the device has ray
// query, and without RCW_RAY_QUERY it is the fallback Hi-Z march over the
// min/max depth pyramid. A ray query hit is shaded from its surface's material
// and lights; a march hit reprojects the previous frame's HDR history.
//
// t_confirmed is the distance proven empty: up to the hit, or the full length
// on a miss, under ray query; the march position after coarse-mip skips under
// the Hi-Z march. A hit deposits its radiance at the cascade containing t;
// every cascade whose cutoff the confirmed distance passed gets a clear. An
// occluder stops the march as unknown: clears up to the confirmed distance,
// nothing beyond. A sky exit that passes RCW_SKY_MIN_FRAC gets clears at every
// cascade. At the top cascade a clear is the sky itself, so only a ray whose
// end is backed by sky deposits one there; one that merely ran out of length
// is occluded beyond range, which counts as opaque: a hit with no radiance.
//
// Deposits are fixed-point integer atomics, so the accumulation is
// order-independent. What is deposited is not run-to-run deterministic:
// Ray generation hands out R2 indices in atomic arrival order, so the direction
// each ray takes varies between runs.
//
// The seed pass runs unordered alongside this one, so this pass must never
// read the probe records or the persistent store.

#include "rc_world_common.slangh"

// Fraction of t_max, about 1/16, a sky exit must have proven empty before its
// continuation counts as sky. Only a screen-edge exit can fall short: a
// full-length or ray query sky exit has proven all of t_max.
static const float RCW_SKY_MIN_FRAC = 16.0 / 255.0;

// March outcomes
static const uint RCW_MARCH_UNKNOWN = 0;
static const uint RCW_MARCH_HIT = 1;
static const uint RCW_MARCH_CLEAR = 2; // full ray length proven empty, geometry behind its end
static const uint RCW_MARCH_SKY = 3;   // confident sky exit, or a full-length clear with sky behind its end

float3 rcwFiniteRadiance(float3 radiance) {
	return all(isfinite(radiance)) ? max(radiance, float3(0.0, 0.0, 0.0)) : float3(0.0, 0.0, 0.0);
}

#ifndef RCW_RAY_QUERY
// The screen-space tracer: a Hi-Z march over the min/max pyramid, with hit
// radiance read from the previous frame's HDR history.

static const int RCW_MAX_MARCH_STEPS = 48;
static const float RCW_CELL_EPS = 0.005; // nudge past the boundary into the next cell
static const float RCW_BACKFACE_DOT = 0.1;
static const float RCW_THICKNESS = 1.0; // view-space hit tolerance, metres

// What bounds the march (s_max).
static const uint RCW_EXIT_FULL_LENGTH = 0; // t_max provable if reached
static const uint RCW_EXIT_CAMERA_WARD = 1; // near plane: nothing beyond is provable
static const uint RCW_EXIT_SCREEN_EDGE = 2; // unknowable; sky-tested at exit

float rcwLinearViewZ(float depth) {
	return push.proj_32 / (-depth - push.proj_22);
}

int2 rcwDepthCoord(float2 uv) {
	return clamp(int2(uv * push.depth_size), int2(0, 0), int2(push.depth_size) - 1);
}

// s at which the ray exits the mip-level cell containing position_xy
float rcwCellExitS(float3 pyramid_start, float3 inverse_segment, float3 segment, float2 position_xy, int level) {
	float cell_size = exp2(float(level));
	float2 cell_min = floor(position_xy / cell_size) * cell_size;
	float2 planes = float2(
		(segment.x >= 0.0) ? cell_min.x + cell_size + RCW_CELL_EPS : cell_min.x - RCW_CELL_EPS,
		(segment.y >= 0.0) ? cell_min.y + cell_size + RCW_CELL_EPS : cell_min.y - RCW_CELL_EPS);
	float2 s_per_axis = (planes - pyramid_start.xy) * inverse_segment.xy;
	return min(s_per_axis.x, s_per_axis.y);
}

// Ray point within RCW_THICKNESS behind the cell's closest surface. hit_depth is
// the per-pixel depth when it agrees with closest_depth, else closest_depth.
bool rcwSlabHit(float3 position, float closest_depth, float2 pyramid_size, out float2 hit_uv, out float hit_depth) {
	hit_uv = position.xy / pyramid_size;
	float texel_depth = depth_texture.Load(int3(rcwDepthCoord(hit_uv), 0));
	hit_depth = closest_depth;
	if (abs(rcwLinearViewZ(texel_depth) - rcwLinearViewZ(closest_depth)) < RCW_THICKNESS)
		hit_depth = texel_depth;
	return position.z <= closest_depth && (rcwLinearViewZ(closest_depth) - rcwLinearViewZ(position.z)) < RCW_THICKNESS;
}

// Radiance of a point a ray stopped at, taken from the previous frame HDR
// history. Zero when the point was off screen last frame, or was not what that
// pixel showed: the stop itself is still known, only its radiance is not.
float3 rcwHistoryRadiance(float3 hit_world) {
	float4 prev_clip = mul(ubo.prev_projection_view, float4(hit_world, 1.0));
	if (prev_clip.w <= 0.0)
		return float3(0.0, 0.0, 0.0);
	float2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
	if (any(prev_uv < 0.0) || any(prev_uv > 1.0))
		return float3(0.0, 0.0, 0.0);
	float3 history_radiance = rcwFiniteRadiance(history_texture.SampleLevel(linear_sampler, prev_uv, 0).rgb);
	history_radiance *= min(1.0, push.luma_clamp / max(rcwLuma(history_radiance), 1e-4));
	if ((push.flags & RCW_FLAG_HIST_DEPTH) != 0u) {
		// The history pixel shows whatever was closest there last frame; the
		// hit owns that radiance only if it was that surface. Reject when the
		// point prev depth falls outside the prev min/max cell envelope
		// (occluded, or revealed/moved): the stop is kept, the radiance zero.
		int2 prev_texel = min(int2(prev_uv * push.depth_size * 0.5),
			int2(push.depth_size * 0.5) - 1);
		float2 prev_depths = prev_depth_texture.Load(int3(prev_texel, 0));
		float view_z_farthest = (prev_depths.x > 0.0) ? rcwLinearViewZ(prev_depths.x) : -1e30;
		float view_z_closest = (prev_depths.y > 0.0) ? rcwLinearViewZ(prev_depths.y) : -1e30;
		float view_z_point = rcwLinearViewZ(prev_clip.z / prev_clip.w);
		if (view_z_point < view_z_farthest - RCW_THICKNESS || view_z_point > view_z_closest + RCW_THICKNESS)
			return float3(0.0, 0.0, 0.0);
	}
	return history_radiance;
}

uint rcwMarch(float3 ray_origin, float3 ray_direction, float t_max,
	out float t_confirmed, out float3 radiance) {
	t_confirmed = 0.0;
	radiance = float3(0.0, 0.0, 0.0);

	float4 clip_start = mul(ubo.projection_view, float4(ray_origin, 1.0));
	float4 clip_end = mul(ubo.projection_view, float4(ray_origin + ray_direction * t_max, 1.0));

	const float MIN_CLIP_W = 1e-4;
	if (clip_start.w < MIN_CLIP_W)
		return RCW_MARCH_UNKNOWN;
	bool end_clipped = false;
	if (clip_end.w < MIN_CLIP_W) {
		clip_end = lerp(clip_start, clip_end, (clip_start.w - MIN_CLIP_W) / (clip_start.w - clip_end.w));
		end_clipped = true;
	}

	float2 pyramid_size = push.depth_size * 0.5;
	float3 ndc_start = clip_start.xyz / clip_start.w;
	float3 ndc_end = clip_end.xyz / clip_end.w;
	float3 pyramid_start = float3((ndc_start.xy * 0.5 + 0.5) * pyramid_size, ndc_start.z);
	float3 pyramid_end = float3((ndc_end.xy * 0.5 + 0.5) * pyramid_size, ndc_end.z);
	float3 segment = pyramid_end - pyramid_start;
	if (max(abs(segment.x), abs(segment.y)) < 1e-4)
		return RCW_MARCH_UNKNOWN; // degenerate screen direction (along the view axis)

	float s_max = 1.0;
	uint exit_kind = RCW_EXIT_FULL_LENGTH;
	if (end_clipped)
		exit_kind = RCW_EXIT_CAMERA_WARD;
	if (pyramid_end.z > 1.0) {
		float s_near_plane = (1.0 - pyramid_start.z) / segment.z;
		if (s_near_plane < s_max) {
			s_max = s_near_plane;
			exit_kind = RCW_EXIT_CAMERA_WARD;
		}
	}
	float2 exit_edge = step(float2(0.0, 0.0), segment.xy) * pyramid_size;
	if (abs(segment.x) > 1e-6) {
		float s_edge_x = (exit_edge.x - pyramid_start.x) / segment.x;
		if (s_edge_x < s_max) {
			s_max = s_edge_x;
			exit_kind = RCW_EXIT_SCREEN_EDGE;
		}
	}
	if (abs(segment.y) > 1e-6) {
		float s_edge_y = (exit_edge.y - pyramid_start.y) / segment.y;
		if (s_edge_y < s_max) {
			s_max = s_edge_y;
			exit_kind = RCW_EXIT_SCREEN_EDGE;
		}
	}
	if (s_max <= 0.0)
		return RCW_MARCH_UNKNOWN;

	float3 inverse_segment;
	inverse_segment.x = 1.0 / ((abs(segment.x) > 1e-8) ? segment.x : (segment.x >= 0.0 ? 1e-8 : -1e-8));
	inverse_segment.y = 1.0 / ((abs(segment.y) > 1e-8) ? segment.y : (segment.y >= 0.0 ? 1e-8 : -1e-8));
	inverse_segment.z = 1.0 / segment.z;

	int max_level = int(push.hiz_mip_count) - 1;
	int level = min(1, max_level);

	// Start at the exit of the origin's cell so no depth test sees the origin
	// surface; that first stretch is unproven
	float s = min(rcwCellExitS(pyramid_start, inverse_segment, segment, pyramid_start.xy, level), s_max);
	// Edge pixels with off-screen-pushed origins start outside the rect: advance
	// to its entry point so the walk never loads negative texels.
	float2 s_rect_low = (float2(0.0, 0.0) - pyramid_start.xy) * inverse_segment.xy;
	float2 s_rect_high = (pyramid_size - pyramid_start.xy) * inverse_segment.xy;
	float s_probe_index = max(min(s_rect_low.x, s_rect_high.x), min(s_rect_low.y, s_rect_high.y));
	if (s_probe_index >= s_max)
		return RCW_MARCH_UNKNOWN;
	s = max(s, s_probe_index);
	float3 position = pyramid_start + s * segment;

	bool hit = false;
	bool unknown = false;
	float2 hit_uv = float2(0.0, 0.0);
	float hit_depth = 0.0;
	float3 hit_normal = float3(0.0, 0.0, 0.0);
	for (int march_step = 0; march_step < RCW_MAX_MARCH_STEPS; march_step++) {
		if (s >= s_max)
			break;
		float cell_size = exp2(float(level));
		int2 cell = int2(position.xy / cell_size);
		// x: farthest surface (min depth), y: closest surface (max depth)
		float2 cell_depths = hiz_pyramid.Load(int3(cell, level));
		float s_exit = min(rcwCellExitS(pyramid_start, inverse_segment, segment, position.xy, level), s_max);
		float z_exit = pyramid_start.z + s_exit * segment.z;

		bool empty = (cell_depths.y <= 0.0) || (min(position.z, z_exit) > cell_depths.y);
		if (empty) {
			s = s_exit;
			position = pyramid_start + s * segment;
			level = min(level + 1, max_level);
			continue;
		}
		// Segment behind the cell's farthest surface by more than RCW_THICKNESS: the ray
		// is inside an occluded region
		if ((rcwLinearViewZ(cell_depths.x) - rcwLinearViewZ(max(position.z, z_exit))) > RCW_THICKNESS) {
			unknown = true;
			break;
		}
		if (segment.z < 0.0) {
			// Away-facing ray: skip to the closest-surface plane; the stretch before it
			// is provably in front of everything in the cell.
			float s_plane = (cell_depths.y - pyramid_start.z) * inverse_segment.z;
			s = clamp(s_plane, s, s_exit);
			position = pyramid_start + s * segment;
		}
		if (level > 0) {
			level--;
			continue;
		}
		if (rcwSlabHit(position, cell_depths.y, pyramid_size, hit_uv, hit_depth)) {
			// The view-depth slab cannot tell entering a surface from grazing above
			// it
			// Validate against the hit pixel's normal
			float3 hit_world = rcwWorldFromUvDepth(hit_uv, hit_depth);
			float3 surface_normal = normal_roughness_texture.Load(int3(rcwDepthCoord(hit_uv), 0)).xyz;
			float normal_length_sq = dot(surface_normal, surface_normal);
			if (normal_length_sq < RCW_MIN_NORMAL_LENGTH_SQ) {
				// A cleared (zero) or coverage-mixed MSAA normal describes no
				// single surface, so it cannot decide the test below. A sky
				// texel did not block the ray, so the cell is passed; anything
				// else is unknown, not proven empty.
				if (depth_texture.Load(int3(rcwDepthCoord(hit_uv), 0)) == 0.0) {
					s = s_exit;
					position = pyramid_start + s * segment;
					level = min(level + 1, max_level);
					continue;
				}
				unknown = true;
				break;
			}
			surface_normal *= rsqrt(normal_length_sq);
			float3 ray_point = rcwWorldFromUvDepth(position.xy / pyramid_size, position.z);
			if (dot(ray_point - hit_world, surface_normal) <= 0.25 * RCW_THICKNESS) {
				hit = true;
				hit_normal = surface_normal;
				break;
			}
			s = s_exit;
			position = pyramid_start + s * segment;
			level = min(level + 1, max_level);
			continue;
		}
		// Level-0 cell the ray dives behind deeper than RCW_THICKNESS: unknown.
		unknown = true;
		break;
	}

	if (hit) {
		float3 hit_world = rcwWorldFromUvDepth(hit_uv, hit_depth);
		t_confirmed = max(dot(hit_world - ray_origin, ray_direction), 0.0);
		if (dot(ray_direction, hit_normal) < RCW_BACKFACE_DOT)
			radiance = rcwHistoryRadiance(hit_world);
		return RCW_MARCH_HIT;
	}

	// Non-hit: everything up to the stop position was proven empty.
	float2 stop_uv = position.xy / pyramid_size;
	float3 stop_world = rcwWorldFromUvDepth(stop_uv, position.z);
	t_confirmed = clamp(dot(stop_world - ray_origin, ray_direction), 0.0, t_max);

	if (unknown)
		return RCW_MARCH_UNKNOWN;
	if (s < s_max)
		return RCW_MARCH_UNKNOWN; // step cap mid-walk
	if (exit_kind == RCW_EXIT_FULL_LENGTH) {
		// Full length proven; snap to t_max
		t_confirmed = t_max;
		// Running out of length proves nothing past t_max. The screen behind
		// the end point is the only evidence available: sky there means the
		// continuation is sky, geometry means it is unknown.
		if (depth_texture.Load(int3(rcwDepthCoord(stop_uv), 0)) == 0.0)
			return RCW_MARCH_SKY;
		return RCW_MARCH_CLEAR;
	}
	if (exit_kind == RCW_EXIT_CAMERA_WARD)
		return RCW_MARCH_UNKNOWN;
	// Screen-edge exit: confident sky only when the ray leaves through a sky
	// region; sliding off screen in front of geometry proves nothing.
	float exit_depth = depth_texture.Load(int3(rcwDepthCoord(stop_uv), 0));
	if (exit_depth == 0.0)
		return RCW_MARCH_SKY;
	return RCW_MARCH_UNKNOWN;
}
#else
#include "rcw_hit_shade.slangh"

// Radiance at a hit is shaded from the surface's own material and lights
// (rcw_hit_shade.slangh), so it is the same wherever the camera points; the
// previous frame's screen, which the screen-space march reads, knows nothing
// off screen.
uint rcwMarchRayQuery(float3 ray_origin, float3 ray_direction, float t_max,
	out float t_confirmed, out float3 radiance) {
	t_confirmed = 0.0;
	radiance = float3(0.0, 0.0, 0.0);

	RayDesc ray;
	ray.Origin = ray_origin;
	ray.Direction = ray_direction;
	ray.TMin = 0.0;
	ray.TMax = t_max;

	RayQuery<RAY_FLAG_NONE> query;
	query.TraceRayInline(rcw_tlas, RAY_FLAG_NONE, 0xFFu, ray);
	// Opaque geometry commits itself. The only candidates handed back are on
	// alpha-masked instances, which are a hit only where the mask is solid.
	while (query.Proceed()) {
		if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
			&& rcwAlphaPasses(query.CandidateInstanceIndex(), query.CandidatePrimitiveIndex(),
				query.CandidateTriangleBarycentrics()))
			query.CommitNonOpaqueTriangleHit();
	}

	if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
		t_confirmed = t_max;
		return RCW_MARCH_SKY;
	}

	t_confirmed = query.CommittedRayT();
	float3 hit_position = ray_origin + ray_direction * t_confirmed;
	RcwHitSurface surface = rcwHitSurfaceOf(query.CommittedInstanceIndex(),
		query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(),
		query.CommittedObjectToWorld3x4(), hit_position, ray_direction);
	radiance = rcwFiniteRadiance(rcwShadeHit(surface));
	radiance *= min(1.0, push.luma_clamp / max(rcwLuma(radiance), 1e-4));
	return RCW_MARCH_HIT;
}
#endif

// One ray's whole contribution to one probe. The mass carries the radiance
// words' RCW_FIXED_ONE scale, which resolve's radiance / mass cancels. Beta is
// the clear share, so only a clear mass tells the merge the ray got through;
// any other lands as a hit (stopped inside the interval, or occluded beyond
// range).
void rcwDeposit(uint cascade, uint probe_index, float3 direction, bool clear, float3 radiance) {
	uint mass = uint(RCW_FIXED_ONE);
	RcwCascadeDesc desc = cascade_descs[cascade];
	uint direction_count = rcwDirCount(cascade);
	uint bin = rcwBinIndex(rcwDirToBin(direction, cascade), cascade);
	uint pair = desc.pair_base + probe_index * direction_count + bin;
	if (clear) {
		InterlockedAdd(scratch_mass[pair].y, mass);
		return;
	}
	// Zero radiance, which every non-hit outcome carries, would add nothing
	if (any(radiance > 0.0)) {
		uint radiance_base = pair * RCW_SCRATCH_RADIANCE_WORDS;
		InterlockedAdd(scratch_radiance[radiance_base + 0], uint(radiance.r * RCW_FIXED_ONE + 0.5));
		InterlockedAdd(scratch_radiance[radiance_base + 1], uint(radiance.g * RCW_FIXED_ONE + 0.5));
		InterlockedAdd(scratch_radiance[radiance_base + 2], uint(radiance.b * RCW_FIXED_ONE + 0.5));
	}
	InterlockedAdd(scratch_mass[pair].x, mass);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	int2 pixel = int2(thread_id.xy);
	if (pixel.x >= int(push.out_size.x) || pixel.y >= int(push.out_size.y))
		return;
	// rcwSpawnPixel is shared with rcw_raygen_count_c0_comp, so the pixels
	// counted there are exactly the pixels that spawn rays here.
	float3 world;
	float3 normal;
	uint lod;
	if (!rcwSpawnPixel(pixel, world, normal, lod)) {
		if (push.debug_gather_mode == RCW_GATHER_TRACE_KIND)
			rc_output[pixel] = float4(0.0);
		return;
	}

	uint parity = uint(push.anchor0_curr.w);
	float ds0_lod = push.ds0 * float(1u << lod);
	// Every ray deposits into the pixel's nearest c0 probe and, per cascade up,
	// the nearest parent of the probe below.
	int3 nearest_grid0 = rcwNearestIndex(rcwCell0(world), int(lod));

	float t_cut[RCW_MAX_CASCADES];
	for (uint cascade = 0; cascade < push.n_cascades; cascade++)
		t_cut[cascade] = rcwCascadeCutoff(cascade, ds0_lod);
	float t_max = t_cut[push.n_cascades - 1];

	// Ray origin offset: push along normal of surface
	float3 view_dir = normalize(world - ubo.camera_position.xyz);
	float view_distance = distance(world, ubo.camera_position.xyz);
	float3 origin = world + normal * (view_distance * (0.002 + 0.004 * (1.0 - saturate(dot(normal, -view_dir)))));

	// Directions: this pixel's rays take the next slots of the contiguous R2
	// segment its cascade-0 probe owns, so a probe's directions are an evenly
	// distributed run rather than a scattered subset. Plus a global 2D jitter
	// per frame so the residual error moves temporally.
	uint jitter_hash = rcwPcgHash(push.frame_counter * RCW_GOLDEN_RATIO_STEP + 0x51EDu); // hashing constant + salt
	float2 jitter = float2(uint2(jitter_hash, rcwPcgHash(jitter_hash)) >> 8) * (1.0 / 16777216.0);
	uint probe0;
	uint rank;
	bool has_probe0 = rcwSpawnProbe0(world, lod, probe0);
	if (has_probe0) {
		InterlockedAdd(ray_cursor[cascade_descs[0].probe_base + probe0], push.rays_per_pixel, rank);
	} else {
		// The probe was dropped this frame (overflow). The rays still trace
		// and still deposit into whatever higher cascades they reach, so
		// they need directions from outside every probe's segment.
		InterlockedAdd(counters[rcwCounterIndex(0u, parity)].raygen_cursor, push.rays_per_pixel, rank);
	}

	static_assert(RCW_RAY_EXIT_CODES == 4, "exit_counts holds one component per RCW_RAY_* code");
	uint4 exit_counts = uint4(0u, 0u, 0u, 0u);
	for (uint ray = 0; ray < push.rays_per_pixel; ray++) {
		// Generate a direction in the hemisphere around the surface normal
		float2 sample_point = frac(rcwR2(rank + ray) + jitter); // Sample in [0,1)^2
		float z = 2.0 * sample_point.y - 1.0;
		// Half-sphere phi on purpose: bin-aligned with rcwDirToBin, so the flip below avoids Algorithm 2's antipodal collisions
		float phi = 3.14159265 * sample_point.x;
		float sin_theta = sqrt(max(1.0 - z * z, 0.0));
		float3 direction = float3(sin_theta * cos(phi), sin_theta * sin(phi), z);
		if (dot(direction, normal) < 0.0)
			direction = -direction;

		float t_confirmed;
		float3 radiance;
		// March the ray from origin along direction
#ifdef RCW_RAY_QUERY
		uint outcome = rcwMarchRayQuery(origin, direction, t_max, t_confirmed, radiance);
#else
		uint outcome = rcwMarch(origin, direction, t_max, t_confirmed, radiance);
#endif

		// Debug visualization
		if (push.debug_gather_mode == RCW_GATHER_TRACE_KIND && ray == 0u) {
			float3 tint = float3(0.0, 0.0, 0.0);
			if (outcome == RCW_MARCH_HIT)
				tint.r = 1.0;
			else if (outcome == RCW_MARCH_UNKNOWN)
				tint.b = 1.0;
			else
				tint.g = 1.0;
			rc_output[pixel] = float4(tint, 0.0);
		}

		// Obtain the cascade that owns the hit: the interval
		// [cut(hit_cascade-1), cut(hit_cascade)) containing t.
		uint hit_cascade = push.n_cascades; // sentinel for no hit
		if (outcome == RCW_MARCH_HIT) {
			hit_cascade = 0;
			while (hit_cascade < push.n_cascades - 1 && t_confirmed >= t_cut[hit_cascade])
				hit_cascade++;
		}
		// A sky exit asserts sky over the whole top interval while only t_confirmed was
		// proven empty; indoors the continuation would have hit an off-screen wall.
		// Require a minimum confirmed fraction.
		bool sky_confirmed = outcome == RCW_MARCH_SKY && t_confirmed >= RCW_SKY_MIN_FRAC * t_max;
		uint exit_code = RCW_RAY_UNKNOWN;
		if (outcome == RCW_MARCH_HIT)
			exit_code = RCW_RAY_HIT;
		else if (outcome == RCW_MARCH_CLEAR)
			exit_code = RCW_RAY_CLEAR;
		else if (sky_confirmed)
			exit_code = RCW_RAY_SKY;
		exit_counts[exit_code]++;

		float t_clear = sky_confirmed ? t_max : t_confirmed;

		// Climbs one parent per cascade; every ray starts the climb from the c0 probe.
		int3 nearest_grid = nearest_grid0;
		for (uint cascade = 0; cascade < push.n_cascades; cascade++) {
			bool is_hit = outcome == RCW_MARCH_HIT && cascade == hit_cascade;
			if (cascade > 0u)
				nearest_grid = rcwNearestParent(nearest_grid);
			if (!is_hit && !(t_clear >= t_cut[cascade]))
				continue;
			// The top cascade has no parent: the merge reads its clear share
			// as the sky reached past the last interval. A ray that only ran
			// out of length proves nothing past t_max: occluded beyond range
			// counts as opaque, so it deposits its mass as a hit.
			bool clear = !is_hit && (cascade < push.n_cascades - 1u || sky_confirmed);
			// rcwSpawnProbe0 already looked up the c0 probe under the same key
			uint probe_index = probe0;
			bool found = has_probe0;
			if (cascade > 0u)
				found = rcwHashLookup(cascade_descs[cascade], parity,
					rcwEncodeKey(nearest_grid, push.anchor0_curr.xyz >> int(cascade + lod), lod), probe_index);
			if (found)
				rcwDeposit(cascade, probe_index, direction, clear, radiance);
		}
	}

	uint4 wave_exits = WaveActiveSum(exit_counts);
	if (WaveIsFirstLane()) {
		uint block = rcwCounterIndex(0u, parity);
		for (uint code = 0; code < RCW_RAY_EXIT_CODES; code++)
			if (wave_exits[code] > 0u)
				InterlockedAdd(counters[block].rays[code], wave_exits[code]);
	}
}
