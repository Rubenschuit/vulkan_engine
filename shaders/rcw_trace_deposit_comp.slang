// Trace + deposit. One thread per spawn pixel, rays_per_pixel rays
// over its upper hemisphere, one Hi-Z march each, the result split into
// per-cascade interval deposits.
//
// A coarse-mip skip proves the segment empty, so the march
// position is the confirmed distance (t_confirmed).
// An occluder stops the march as unknown: clears up to the confirmed
// distance, nothing beyond. A hit deposits reprojected history radiance at
// the cascade containing t; every cascade whose cutoff the confirmed
// distance passed gets a clear.
// A sky exit that passes rcwSkyMinFrac() gets clears at every cascade
// At the top cascade a clear is the sky itself, so only a ray whose end is
// backed by sky on screen deposits one there; one that merely ran out of
// length deposits mass with no clear share.
//
// Deposits are fixed-point integer atomics: commutative addition and
// bit-deterministic while the overflow counter is 0.

#include "rc_world_common.slangh"

static const int RCW_MAX_MARCH_STEPS = 48;
static const float RCW_CELL_EPS = 0.005; // nudge past the boundary into the next cell
static const float RCW_BACKFACE_DOT = 0.1;

// March outcomes
static const uint RCW_MARCH_UNKNOWN = 0;
static const uint RCW_MARCH_HIT = 1;
static const uint RCW_MARCH_CLEAR = 2; // full ray length proven empty
static const uint RCW_MARCH_SKY = 3;   // confident sky exit

// What bounds the march (s_max).
static const uint RCW_EXIT_FULL_LENGTH = 0; // t_max provable if reached
static const uint RCW_EXIT_CAMERA_WARD = 1; // near plane: nothing beyond is provable
static const uint RCW_EXIT_SCREEN_EDGE = 2; // unknowable; sky-tested at exit

float rcwLinearViewZ(float depth) {
	return push.proj_32 / (-depth - push.proj_22);
}

float3 rcwFiniteRadiance(float3 radiance) {
	return all(isfinite(radiance)) ? max(radiance, float3(0.0, 0.0, 0.0)) : float3(0.0, 0.0, 0.0);
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

// Ray point within thickness behind the cell's closest surface. hit_depth is
// the per-pixel depth when it agrees with closest_depth, else closest_depth.
bool rcwSlabHit(float3 position, float closest_depth, float2 pyramid_size, out float2 hit_uv, out float hit_depth) {
	hit_uv = position.xy / pyramid_size;
	float texel_depth = depth_texture.Load(int3(rcwDepthCoord(hit_uv), 0));
	hit_depth = closest_depth;
	if (abs(rcwLinearViewZ(texel_depth) - rcwLinearViewZ(closest_depth)) < push.thickness)
		hit_depth = texel_depth;
	return position.z <= closest_depth && (rcwLinearViewZ(closest_depth) - rcwLinearViewZ(position.z)) < push.thickness;
}

uint rcwMarch(float3 ray_origin, float3 ray_direction, float t_max,
	out float t_confirmed, out float3 radiance, out uint cause, out bool sky_beyond) {
	t_confirmed = 0.0;
	radiance = float3(0.0, 0.0, 0.0);
	cause = RCW_RAY_UNK_CLIP;
	sky_beyond = false;

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
	if (s_max <= 0.0) {
		if (exit_kind == RCW_EXIT_SCREEN_EDGE)
			cause = RCW_RAY_UNK_EDGE;
		return RCW_MARCH_UNKNOWN;
	}

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
	if (s_probe_index >= s_max) {
		if (exit_kind == RCW_EXIT_SCREEN_EDGE)
			cause = RCW_RAY_UNK_EDGE;
		return RCW_MARCH_UNKNOWN;
	}
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
		// Segment behind the cell's farthest surface by more than thickness: the ray
		// is inside an occluded region
		if ((rcwLinearViewZ(cell_depths.x) - rcwLinearViewZ(max(position.z, z_exit))) > push.thickness) {
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
			float3 surface_normal = normalize(normal_roughness_texture.Load(
				int3(rcwDepthCoord(hit_uv), 0)).xyz);
			float3 ray_point = rcwWorldFromUvDepth(position.xy / pyramid_size, position.z);
			if (dot(ray_point - hit_world, surface_normal) <= 0.25 * push.thickness) {
				hit = true;
				hit_normal = surface_normal;
				break;
			}
			s = s_exit;
			position = pyramid_start + s * segment;
			level = min(level + 1, max_level);
			continue;
		}
		// Level-0 cell the ray dives behind deeper than thickness: unknown.
		unknown = true;
		break;
	}

	if (hit) {
		float3 hit_world = rcwWorldFromUvDepth(hit_uv, hit_depth);
		t_confirmed = max(dot(hit_world - ray_origin, ray_direction), 0.0);

		// History-OOB and back-side hits keep the hit with zero radiance: occlusion
		// is known even when the radiance is not.
		float4 prev_clip = mul(ubo.prev_projection_view, float4(hit_world, 1.0));
		if (prev_clip.w > 0.0 && dot(ray_direction, hit_normal) < RCW_BACKFACE_DOT) {
			float2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
			if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
				float3 history_radiance = rcwFiniteRadiance(history_texture.SampleLevel(linear_sampler, prev_uv, 0).rgb) * push.intensity;
				history_radiance *= min(1.0, push.luma_clamp / max(rcwLuma(history_radiance), 1e-4));
				if ((push.flags & RCW_FLAG_HIST_DEPTH) != 0u) {
					// The history pixel shows whatever was closest there last frame; the
					// hit owns that radiance only if it was that surface. Reject when the
					// point's prev depth falls outside the prev min/max cell's envelope
					// (occluded, or revealed/moved): hit kept, radiance zero.
					int2 prev_texel = min(int2(prev_uv * push.depth_size * 0.5),
						int2(push.depth_size * 0.5) - 1);
					float2 prev_depths = prev_depth_texture.Load(int3(prev_texel, 0));
					float view_z_farthest = (prev_depths.x > 0.0) ? rcwLinearViewZ(prev_depths.x) : -1e30;
					float view_z_closest = (prev_depths.y > 0.0) ? rcwLinearViewZ(prev_depths.y) : -1e30;
					float view_z_point = rcwLinearViewZ(prev_clip.z / prev_clip.w);
					if (view_z_point < view_z_farthest - push.thickness || view_z_point > view_z_closest + push.thickness)
						history_radiance = float3(0.0, 0.0, 0.0);
				}
				radiance = history_radiance;
			}
		}
		cause = RCW_RAY_HIT;
		return RCW_MARCH_HIT;
	}

	// Non-hit: everything up to the stop position was proven empty.
	float2 stop_uv = position.xy / pyramid_size;
	float3 stop_world = rcwWorldFromUvDepth(stop_uv, position.z);
	t_confirmed = clamp(dot(stop_world - ray_origin, ray_direction), 0.0, t_max);

	if (unknown) {
		cause = RCW_RAY_UNK_OCCLUDED;
		return RCW_MARCH_UNKNOWN;
	}
	if (s < s_max) {
		cause = RCW_RAY_UNK_STEPCAP;
		return RCW_MARCH_UNKNOWN; // step cap mid-walk
	}
	if (exit_kind == RCW_EXIT_FULL_LENGTH) {
		// Full length proven; snap to t_max
		t_confirmed = t_max;
		cause = RCW_RAY_CLEAR;
		// Running out of length proves nothing past t_max. The screen behind
		// the end point is the only evidence available: sky there means the
		// continuation is sky, geometry means it is unknown.
		sky_beyond = depth_texture.Load(int3(rcwDepthCoord(stop_uv), 0)) == 0.0;
		return RCW_MARCH_CLEAR;
	}
	if (exit_kind == RCW_EXIT_CAMERA_WARD)
		return RCW_MARCH_UNKNOWN;
	// Screen-edge exit: confident sky only when the ray leaves through a sky
	// region; sliding off screen in front of geometry proves nothing.
	float exit_depth = depth_texture.Load(int3(rcwDepthCoord(stop_uv), 0));
	cause = RCW_RAY_UNK_EDGE;
	if (exit_depth == 0.0) {
		cause = RCW_RAY_SKY;
		sky_beyond = true;
		return RCW_MARCH_SKY;
	}
	return RCW_MARCH_UNKNOWN;
}

// Which word of the bin a ray's mass lands in. Beta is the clear share, so
// only RCW_MASS_CLEAR tells the merge the ray got through.
static const uint RCW_MASS_HIT = 0;     // radiance found inside the interval
static const uint RCW_MASS_UNKNOWN = 1; // no radiance and no claim on the interval
static const uint RCW_MASS_CLEAR = 2;

// One ray's contribution to one probe, scaled by the probe's share of the ray.
void rcwDeposit(uint cascade, uint probe_index, float3 direction, uint mass_kind, float3 radiance, float share) {
	uint mass = uint(share * RCW_FIXED_ONE + 0.5);
	if (mass == 0u)
		return;
	RcwCascadeDesc desc = cascade_descs[cascade];
	uint direction_count = rcwDirCount(cascade);
	uint bin = rcwBinIndex(rcwDirToBin(direction, cascade), cascade);
	uint scratch_base = (desc.pair_base + probe_index * direction_count + bin) * RCW_SCRATCH_WORDS;
	if (mass_kind == RCW_MASS_CLEAR) {
		InterlockedAdd(scratch[scratch_base + 4], mass);
		return;
	}
	if (mass_kind == RCW_MASS_HIT) {
		InterlockedAdd(scratch[scratch_base + 0], uint(radiance.r * share * RCW_FIXED_ONE + 0.5));
		InterlockedAdd(scratch[scratch_base + 1], uint(radiance.g * share * RCW_FIXED_ONE + 0.5));
		InterlockedAdd(scratch[scratch_base + 2], uint(radiance.b * share * RCW_FIXED_ONE + 0.5));
	}
	InterlockedAdd(scratch[scratch_base + 3], mass);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	int2 pixel = int2(thread_id.xy);
	if (pixel.x >= int(push.out_size.x) || pixel.y >= int(push.out_size.y))
		return;
	float3 world;
	uint lod;
	float lod_blend;
	if (!rcwPixelWorld(pixel, world, lod, lod_blend))
		return;
	// Blend zone: pick one lod stochastically per (pixel, frame)
	if (lod_blend > 0.0 && lod < RCW_MAX_LOD) {
		uint lod_rng = rcwRngSeed(pixel, push.frame_counter ^ 0xB1E4Du);
		if (rcwRandFloat(lod_rng) < lod_blend)
			lod++;
	}

	int2 depth_texel = rcwDepthTexel(pixel);
	float3 normal_raw = normal_roughness_texture.Load(int3(depth_texel, 0)).xyz;
	// MSAA eAverage can cancel opposing normals to zero
	if (dot(normal_raw, normal_raw) < 1e-12) {
		if (push.debug_gather_mode == RCW_GATHER_TRACE_KIND)
			rc_output[pixel] = float4(0.0);
		return;
	}
	float3 normal = normalize(normal_raw);

	uint parity = uint(push.anchor0_curr.w);
	int3 cell0 = rcwCell0(world);
	float ds0_lod = push.ds0 * float(1u << lod);

	uint deposit_mode = rcwDepositMode();
	// Weighted mode deposits into all 8 corners
	bool weighted = deposit_mode == RCW_DEPOSIT_WEIGHTED;
	// Nearest mode deposits into the pixel's nearest c0 probe and, per cascade
	// up, the nearest parent of the probe below.
	bool nearest = deposit_mode == RCW_DEPOSIT_NEAREST;
	int3 nearest_grid = int3(0, 0, 0);
	if (nearest)
		nearest_grid = rcwNearestIndex(cell0, int(lod));

	float t_cut[RCW_MAX_CASCADES];
	for (uint cascade = 0; cascade < push.n_cascades; cascade++)
		t_cut[cascade] = rcwCascadeCutoff(cascade, ds0_lod);
	float t_max = t_cut[push.n_cascades - 1];

	// Ray origin offset: push along normal of surface
	float3 view_dir = normalize(world - ubo.camera_position.xyz);
	float view_distance = distance(world, ubo.camera_position.xyz);
	float3 origin = world + normal * (view_distance * (0.002 + 0.004 * (1.0 - saturate(dot(normal, -view_dir)))));

	// Directions: R2 sampling by the pixel's Morton rank plus a global (frame
	// count based) 2D jitter to improve temporal accumulation.
	uint jitter_hash = rcwPcgHash(push.frame_counter * RCW_GOLDEN_RATIO_STEP + 0x51EDu); // hashing constant + salt
	float2 jitter = float2(uint2(jitter_hash, rcwPcgHash(jitter_hash)) >> 8) * (1.0 / 16777216.0);
	uint rank = rcwMorton2D(uint2(pixel)) * push.rays_per_pixel;

	for (uint ray = 0; ray < push.rays_per_pixel; ray++) {
		// Generate a direction in the hemisphere around the surface normal
		float2 sample_point = frac(rcwR2(rank + ray) + jitter); // Sample in [0,1)^2
		float z = 2.0 * sample_point.y - 1.0;
		float phi = 3.14159265 * sample_point.x;
		float sin_theta = sqrt(max(1.0 - z * z, 0.0));
		float3 direction = float3(sin_theta * cos(phi), sin_theta * sin(phi), z);
		if (dot(direction, normal) < 0.0)
			direction = -direction;

		float t_confirmed;
		float3 radiance;
		uint cause;
		bool sky_beyond;
		// March the ray from origin along direction
		uint outcome = rcwMarch(origin, direction, t_max, t_confirmed, radiance, cause, sky_beyond);

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
		bool sky_confirmed = (outcome == RCW_MARCH_SKY
			&& t_confirmed >= rcwSkyMinFrac() * t_max)
			|| (outcome == RCW_MARCH_CLEAR && sky_beyond);
		if (outcome == RCW_MARCH_SKY && !sky_confirmed)
			cause = RCW_RAY_SKY_GATED;
		InterlockedAdd(ray_stats[parity * RCW_RAY_STAT_WORDS + cause], 1u);

		float t_clear = sky_confirmed ? t_max : t_confirmed;

		for (uint cascade = 0; cascade < push.n_cascades; cascade++) {
			bool is_hit = outcome == RCW_MARCH_HIT && cascade == hit_cascade;
			if (nearest && cascade > 0u)
				nearest_grid = rcwNearestParent(nearest_grid);
			if (!is_hit && !(t_clear >= t_cut[cascade]))
				continue;
			// The top cascade has no parent: the merge reads its clear share
			// as the sky reached past the last interval. A ray that only ran
			// out of length proves nothing past t_max, so it deposits its mass
			// without a clear share.
			uint mass_kind = is_hit ? RCW_MASS_HIT : RCW_MASS_CLEAR;
			if (!is_hit && cascade == push.n_cascades - 1u && !sky_confirmed)
				mass_kind = RCW_MASS_UNKNOWN;
			int scale = int(cascade + lod);
			float3 fraction;
			int3 cell = rcwProbeBase(world, scale, fraction);
			int3 anchor = push.anchor0_curr.xyz >> scale;
			if (nearest) {
				uint nearest_probe_index;
				if (rcwHashLookup(cascade_descs[cascade], parity,
					rcwEncodeKey(nearest_grid, anchor, lod), nearest_probe_index))
					rcwDeposit(cascade, nearest_probe_index, direction, mass_kind, radiance, 1.0);
				continue;
			}
			uint corner_count = weighted ? 8u : 1u;
			for (uint corner_index = 0; corner_index < corner_count; corner_index++) {
				int3 corner;
				float share = 1.0;
				if (weighted) {
					corner = rcwCorner(corner_index);
					share = rcwCornerWeight(corner_index, fraction);
					if (share * RCW_FIXED_ONE < 0.5)
						continue;
				} else {
					uint rng = rcwRngSeed(pixel, push.frame_counter * 16u + cascade);
					corner.x = (rcwRandFloat(rng) < fraction.x) ? 1 : 0;
					corner.y = (rcwRandFloat(rng) < fraction.y) ? 1 : 0;
					corner.z = (rcwRandFloat(rng) < fraction.z) ? 1 : 0;
				}
				uint probe_index;
				if (!rcwHashLookup(cascade_descs[cascade], parity,
					rcwEncodeKey(cell + corner, anchor, lod), probe_index))
					continue;
				rcwDeposit(cascade, probe_index, direction, mass_kind, radiance, share);
			}
		}
	}
}
