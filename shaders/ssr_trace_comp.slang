// Screen-space reflections trace.
// Hi-Z traversal: walks the SsrHizPyramid min/max depth pyramid (built from
// the current-frame resolved depth) along the reflection ray in
// post-projective space, where the ray is a straight line in (uv, ndc_z) and
// empty cells can be skipped at coarse mips without stepping over thin
// geometry. Rays are built from the prepass G-buffer normal; radiance comes
// from the previous frame's HDR history, reprojected via
// ubo.prev_projection_view (camera-only, so moving objects ghost slightly).
// Output: rgb radiance + confidence in alpha; confidence 0 falls back to the
// IBL cubemap in the composite.

// The traversal works in "pyr0 space": xy = continuous mip-0 pyramid texel
// coordinates (full-res depth pixel (x,y) maps to texel (x/2, y/2); mip-L
// cell boundaries are multiples of 2^L), z = reverse-Z NDC depth. The ray is
//
// p(s) = s0 + s * direction, with s in [0, s_max].
//
// The parameter s is linear in screen-xy and ndc_z, so the ray is a straight
// line in pyr0 space.

#include "ve_common.slangh"

//#define SSR_DEBUG_HEATMAP

[vk::binding(0, 1)]
Texture2D<float> depth_texture;
[vk::binding(1, 1)]
Texture2D<float4> normal_roughness_texture;
[vk::binding(2, 1)]
Texture2D<float4> history_texture;
[vk::binding(3, 1)]
SamplerState history_sampler;
[vk::binding(4, 1)]
RWTexture2D<float4> ssr_out;
[vk::binding(5, 1)]
Texture2D<float2> hiz_pyramid;  // .x = min (farthest), .y = max (closest); (0,0) = empty

struct SsrPush {
	float2 out_size;
	float2 depth_size;
	float proj_22;
	float proj_32;
	float thickness;      // view-space hit tolerance
	float max_roughness;  // no reflections above this roughness
	float max_distance;   // world-space ray length
	int max_steps;        // traversal iteration cap
	uint hiz_mip_count;
	float _pad;
};
[[vk::push_constant]] SsrPush push;

// Reverse-Z view-space depth
float linearViewZ(float depth) {
	return push.proj_32 / (-depth - push.proj_22);
}

int2 depthCoord(float2 uv) {
	return min(int2(uv * push.depth_size), int2(push.depth_size) - 1);
}

float3 worldFromUvDepth(float2 uv, float depth) {
	float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
	float4 world = mul(ubo.inverse_projection_view, ndc);
	return world.xyz / world.w;
}

// nudge past the boundary so the next step lands in the next cell
static const float CELL_EPS = 0.005;

static const float MAX_REFLECTED_LUMA = 10.0;

// s at which the ray exits the mip-level cell containing p_xy
float cellExitS(float3 s0, float3 inv_d, float3 d, float2 p_xy, int level) {
	float cs = exp2(float(level));
	float2 cell_min = floor(p_xy / cs) * cs;
	float2 planes = float2(
		(d.x >= 0.0) ? cell_min.x + cs + CELL_EPS : cell_min.x - CELL_EPS,
		(d.y >= 0.0) ? cell_min.y + cs + CELL_EPS : cell_min.y - CELL_EPS);
	float2 s2 = (planes - s0.xy) * inv_d.xy;
	return min(s2.x, s2.y);
}

// True when the ray point p(s) = s0 + s * d is within 'thickness' behind the
// cell's closest surface (slab). scene_d is set to the sharp per-pixel depth when
// it agrees with cell_max, else falls back to cell_max.
bool slabHit(float3 p, float cell_max, float2 uv_to_pyr, out float2 hit_uv, out float scene_d) {
	hit_uv = p.xy / uv_to_pyr;
	float texel_d = depth_texture.Load(int3(depthCoord(hit_uv), 0));
	scene_d = cell_max;
	if (abs(linearViewZ(texel_d) - linearViewZ(cell_max)) < push.thickness)
		scene_d = texel_d;
	return p.z <= cell_max && (linearViewZ(cell_max) - linearViewZ(p.z)) < push.thickness;
}

[numthreads(16, 16, 1)]
void compMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= uint(push.out_size.x) || id.y >= uint(push.out_size.y)) return;

	float2 uv = (float2(id.xy) + 0.5) / push.out_size;
	int2 depth_coord = depthCoord(uv);

	float depth = depth_texture.Load(int3(depth_coord, 0));
	float4 nr = normal_roughness_texture.Load(int3(depth_coord, 0));
	float roughness = nr.w;
	if (depth <= 0.0 || roughness > push.max_roughness) {
		ssr_out[id.xy] = float4(0, 0, 0, 0);
		return;
	}

	// Origin snapped to the loaded pixel's center so the ray depth matches
	// the pyramid's value for that pixel (at half-res the raw uv sits on a
	// 4-pixel corner while the Load picks one of them)
	float2 uv0 = (float2(depth_coord) + 0.5) / push.depth_size;
	float3 world_pos = worldFromUvDepth(uv0, depth);
	float3 normal = normalize(nr.xyz);
	float3 view_dir = normalize(world_pos - ubo.camera_position.xyz);
	float3 ray_dir = reflect(view_dir, normal);

	// Start the ray off its own surface: offset grows with view distance and
	// at grazing angles
	float dist0 = -linearViewZ(depth);
	float nb = dist0 * (0.002 + 0.004 * (1.0 - saturate(dot(normal, -view_dir))));
	float3 ray_org = world_pos + normal * nb;

	// Project both segment endpoints, clipping against w > eps first so a
	// toward-camera segment never crosses the camera plane
	float4 c0 = mul(ubo.projection_view, float4(ray_org, 1.0));
	float4 c1 = mul(ubo.projection_view, float4(ray_org + ray_dir * push.max_distance, 1.0));
	const float W_EPS = 1e-4;
	if (c1.w < W_EPS)
		c1 = lerp(c0, c1, (c0.w - W_EPS) / (c0.w - c1.w));

	float2 uv_to_pyr = push.depth_size * 0.5;
	float3 ndc0 = c0.xyz / c0.w;
	float3 ndc1 = c1.xyz / c1.w;
	float3 s0 = float3((ndc0.xy * 0.5 + 0.5) * uv_to_pyr, ndc0.z);
	float3 s1 = float3((ndc1.xy * 0.5 + 0.5) * uv_to_pyr, ndc1.z);
	float3 d = s1 - s0;

	// Degenerate screen direction: the ray mirrors the camera itself, which
	// is never in the history
	if (max(abs(d.x), abs(d.y)) < 1e-4) {
		ssr_out[id.xy] = float4(0, 0, 0, 0);
		return;
	}

	// slangc 2026.1 miscompiles a ternary nested in the min() in the s_max
	// assignments below, zeroing s_max for up-screen rays (killed all floor reflections)
	float s_max = 1.0;
	if (s1.z > 1.0)
		s_max = min(s_max, (1.0 - s0.z) / d.z);
	float2 rect_edge = step(float2(0.0, 0.0), d.xy) * uv_to_pyr;
	if (abs(d.x) > 1e-6)
		s_max = min(s_max, (rect_edge.x - s0.x) / d.x);
	if (abs(d.y) > 1e-6)
		s_max = min(s_max, (rect_edge.y - s0.y) / d.y);

	float3 inv_d;
	inv_d.x = 1.0 / ((abs(d.x) > 1e-8) ? d.x : (d.x >= 0.0 ? 1e-8 : -1e-8));
	inv_d.y = 1.0 / ((abs(d.y) > 1e-8) ? d.y : (d.y >= 0.0 ? 1e-8 : -1e-8));
	inv_d.z = 1.0 / d.z;

	int max_level = int(push.hiz_mip_count) - 1;
	int level = min(1, max_level);

	// Start at the exit of the origin's cell so no depth test ever sees the
	// origin surface itself
	float s = min(cellExitS(s0, inv_d, d, s0.xy, level), s_max);
	float3 p = s0 + s * d;

	bool hit = false;
	float2 hit_uv = float2(0.0);
	float scene_d = 0.0;
	int i = 0;
	for (; i < push.max_steps; i++) {
		if (s >= s_max)
			break;

		float cs = exp2(float(level));
		int2 cell = int2(p.xy / cs);
		float2 zz = hiz_pyramid.Load(int3(cell, level));

		float s_exit = min(cellExitS(s0, inv_d, d, p.xy, level), s_max);
		float z_exit = s0.z + s_exit * d.z;

		// The cell may contain a hit if any part of the segment reaches
		// at/behind the closest surface (.y); .y == 0 marks sky/padding.
		// z is linear in s, so the interval extrema are its endpoints.
		bool may_hit = (zz.y > 0.0) && (min(p.z, z_exit) <= zz.y);
		// Skip-behind via the .x (farthest) channel
		//may_hit = may_hit && !(max(p.z, z_exit) < zz.x);

		if (!may_hit) {
			s = s_exit;
			p = s0 + s * d;
			level = min(level + 1, max_level);
			continue;
		}

		if (d.z < 0.0) {
			// Away-facing ray
			float s_plane = (zz.y - s0.z) * inv_d.z;
			s = clamp(s_plane, s, s_exit);
			p = s0 + s * d;
		}
		if (level > 0) {
			level--;
			continue;
		}

		if (slabHit(p, zz.y, uv_to_pyr, hit_uv, scene_d)) {
			hit = true;
			break;
		}
		// Outside the slab (in front, or behind by more than thickness):
		// step through and re-coarsen
		s = s_exit;
		p = s0 + s * d;
		level = min(level + 1, max_level);
	}

	// Iteration cap mid-descent: give the final position one validation
	// chance
	if (!hit && i >= push.max_steps && s < s_max) {
		float cap_max = hiz_pyramid.Load(int3(int2(p.xy), 0)).y;
		if (cap_max > 0.0)
			hit = slabHit(p, cap_max, uv_to_pyr, hit_uv, scene_d);
	}

#ifdef SSR_DEBUG_HEATMAP
	{
		// Iteration heatmap: blue = cheap, green = mid, red = at cap
		float t = float(i) / float(push.max_steps);
		ssr_out[id.xy] = float4(t, 1.0 - abs(2.0 * t - 1.0), 1.0 - t, 1.0);
		return;
	}
#endif

	if (!hit) {
		ssr_out[id.xy] = float4(0, 0, 0, 0);
		return;
	}

	// Fetch last frame's radiance at the actual surface point (camera-only reprojection)
	float3 hit_world = worldFromUvDepth(hit_uv, scene_d);
	float4 prev_clip = mul(ubo.prev_projection_view, float4(hit_world, 1.0));
	if (prev_clip.w <= 0.0) {
		ssr_out[id.xy] = float4(0, 0, 0, 0);
		return;
	}
	float2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
	if (any(prev_uv < 0.0) || any(prev_uv > 1.0)) {
		ssr_out[id.xy] = float4(0, 0, 0, 0);
		return;
	}
	float hit_dist = distance(hit_world, world_pos);
	float travel = distance(world_pos, ubo.camera_position.xyz) + hit_dist;
	float footprint = travel / max(-linearViewZ(scene_d), 1e-3) * (push.depth_size.y / push.out_size.y);
	float3 color = history_texture.SampleLevel(history_sampler, prev_uv, log2(max(footprint, 1.0))).rgb;

	// Soft firefly clamp
	float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
	color *= MAX_REFLECTED_LUMA / max(MAX_REFLECTED_LUMA, luma);

	float2 border = saturate((0.5 - abs(hit_uv - 0.5)) / 0.1);
	float edge_fade = border.x * border.y;
	float2 prev_border = saturate((0.5 - abs(prev_uv - 0.5)) / 0.1);
	float history_fade = prev_border.x * prev_border.y;
	float dist_fade = 1.0 - saturate(hit_dist / push.max_distance);
	float rough_fade = 1.0 - smoothstep(push.max_roughness * 0.7, max(push.max_roughness, 1e-4), roughness);
	float confidence = edge_fade * history_fade * dist_fade * rough_fade;

	ssr_out[id.xy] = float4(color, confidence);
}
