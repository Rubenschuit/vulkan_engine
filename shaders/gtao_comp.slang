// Ground Truth Ambient Occlusion (GTAO) compute shader.
// Jimenez 2016 — horizon-based AO with analytical integration.
// Runs after the geometry prepass, on the graphics or async compute queue.
// Reads single-sample resolved depth plus the prepass G-buffer normal+roughness texture.

#include "ve_common.slangh"

#define GTAO_PI 3.1415926536

// Set 1: Compute I/O
[vk::binding(0, 1)]
Texture2D<float> depth_texture;
[vk::binding(1, 1)]
RWTexture2D<float> ao_out;
[vk::binding(2, 1)]
Texture2D<float4> normal_roughness_texture;

struct GtaoPush {
	float2 ao_size;
	float2 depth_size;
	float radius;
	float intensity;
	float proj_scale;
	float inv_focal_x;   // 1.0 / proj[0][0]
	float inv_focal_y;   // 1.0 / proj[1][1]
	float proj_22;       // proj[2][2] for depth linearization
	float proj_32;       // proj[3][2] for depth linearization
	float _pad;
};
[[vk::push_constant]] GtaoPush push;

// Reconstruct view-space position from pixel coordinate and depth
float3 getViewPos(int2 coord, float depth) {
	float2 uv = (float2(coord) + 0.5) / push.depth_size;
	float2 ndc = uv * 2.0 - 1.0;

	// Linearize depth: view_z = proj_32 / (-depth - proj_22)
	float view_z = push.proj_32 / (-depth - push.proj_22);

	// Reconstruct XY from NDC
	return float3(
		ndc.x * (-view_z) * push.inv_focal_x,
		ndc.y * (-view_z) * push.inv_focal_y,
		view_z
	);
}

// Fetch depth at pixel coordinate
float fetchDepth(int2 coord) {
	coord = clamp(coord, int2(0), int2(push.depth_size) - 1);
	return depth_texture.Load(int3(coord, 0));
}

static const uint NUM_SLICES = 4;
static const uint NUM_STEPS  = 4;

// Cosine between the view vector and the direction to the surface at coord,
// faded toward low_cos (the hemisphere boundary, no occlusion) as the sample
// leaves the radius. Sky and out-of-radius samples occlude nothing.
float horizonCos(int2 coord, float3 view_pos, float3 view_vec, float low_cos) {
	coord = clamp(coord, int2(0), int2(push.depth_size) - 1);
	float sample_depth = fetchDepth(coord);
	if (sample_depth <= 0.0)
		return low_cos;
	float3 delta = getViewPos(coord, sample_depth) - view_pos;
	float len = length(delta);
	if (len < 0.001)
		return low_cos;
	float falloff = saturate(1.0 - len / (push.radius * 2.0));
	return lerp(low_cos, dot(delta / len, view_vec), falloff);
}

[numthreads(16, 16, 1)]
void compMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= uint(push.ao_size.x) || id.y >= uint(push.ao_size.y)) return;

	// Map AO pixel to depth pixel (may differ if half-res)
	float2 uv = (float2(id.xy) + 0.5) / push.ao_size;
	int2 depth_coord = int2(uv * push.depth_size);

	float depth = fetchDepth(depth_coord);
	if (depth <= 0.0) {
		ao_out[id.xy] = 1.0;
		return;
	}

	float3 view_pos = getViewPos(depth_coord, depth);
	float3 world_normal = normal_roughness_texture.Load(int3(depth_coord, 0)).xyz;
	float3 view_normal = normalize(mul((float3x3)ubo.view, world_normal));
	float3 view_vec = normalize(-view_pos);

	// Screen-space radius from world-space radius
	float ss_radius = push.proj_scale * push.radius / abs(view_pos.z);
	ss_radius = max(ss_radius, 3.0); // minimum 3 pixels
	ss_radius = min(ss_radius, min(push.ao_size.x, push.ao_size.y) * 0.5);

	float noise = spatialNoise(id.xy);
	float visibility = 0.0;

	for (uint slice = 0; slice < NUM_SLICES; slice++) {
		// Evenly distributed angle + noise offset
		float phi = (GTAO_PI / float(NUM_SLICES)) * (float(slice) + noise);
		float2 dir = float2(cos(phi), sin(phi));
		// The step direction in view space: proj[1][1] < 0 (Vulkan Y-flip), so
		// screen y runs opposite to view y
		float3 dir_view = float3(dir.x, -dir.y, 0.0);

		// The slice plane holds the view vector and the step direction; the
		// normal enters projected into it, its angle n measured from the view vector
		float3 ortho_dir = dir_view - dot(dir_view, view_vec) * view_vec;
		float3 axis = normalize(cross(ortho_dir, view_vec));
		float3 proj_normal = view_normal - axis * dot(view_normal, axis);
		float proj_normal_len = length(proj_normal);
		float cos_n = saturate(dot(proj_normal, view_vec) / max(proj_normal_len, 1e-4));
		float n = sign(dot(ortho_dir, proj_normal)) * acos(cos_n);

		// Both horizons start on the hemisphere boundary around the normal
		float low_cos_pos = cos(n + GTAO_PI * 0.5);
		float low_cos_neg = cos(n - GTAO_PI * 0.5);
		float horizon_cos_pos = low_cos_pos;
		float horizon_cos_neg = low_cos_neg;

		for (uint step = 1; step <= NUM_STEPS; step++) {
			float t = (float(step) + noise * 0.5) / float(NUM_STEPS);
			int2 offset = int2(dir * ss_radius * t * (push.depth_size / push.ao_size));
			horizon_cos_pos = max(horizon_cos_pos, horizonCos(depth_coord + offset, view_pos, view_vec, low_cos_pos));
			horizon_cos_neg = max(horizon_cos_neg, horizonCos(depth_coord - offset, view_pos, view_vec, low_cos_neg));
		}

		// Signed horizon angles from the view vector, kept within the hemisphere
		float h_pos = acos(clamp(horizon_cos_pos, -1.0, 1.0));
		float h_neg = -acos(clamp(horizon_cos_neg, -1.0, 1.0));
		h_pos = n + clamp(h_pos - n, -GTAO_PI * 0.5, GTAO_PI * 0.5);
		h_neg = n + clamp(h_neg - n, -GTAO_PI * 0.5, GTAO_PI * 0.5);

		// Cosine-weighted visibility of each half-slice (Jimenez 2016, eq. 10),
		// weighted by how much of the normal lies in the slice
		float sin_n = sin(n);
		float arc_pos = 0.25 * (cos_n + 2.0 * h_pos * sin_n - cos(2.0 * h_pos - n));
		float arc_neg = 0.25 * (cos_n + 2.0 * h_neg * sin_n - cos(2.0 * h_neg - n));
		visibility += proj_normal_len * (arc_pos + arc_neg);
	}

	float ao = saturate(visibility / float(NUM_SLICES));
	ao = pow(ao, push.intensity);

	ao_out[id.xy] = ao;
}
