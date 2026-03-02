// Ground Truth Ambient Occlusion (GTAO) compute shader.
// Jimenez 2016 — horizon-based AO with analytical integration.
// Dispatched inline on graphics queue after depth prepass.
// Always reads single-sample depth (resolved from MSAA if active).

#include "ve_constants.slangh"

#define GTAO_PI 3.1415926536

// Set 1: Compute I/O
[vk::binding(0, 1)]
Texture2D<float> depth_texture;
[vk::binding(1, 1)]
RWTexture2D<float> ao_out;

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

// Reconstruct view-space normal from depth buffer cross products
float3 reconstructNormal(int2 coord, float3 view_pos) {
	float3 left  = getViewPos(coord + int2(-1, 0), fetchDepth(coord + int2(-1, 0)));
	float3 right = getViewPos(coord + int2( 1, 0), fetchDepth(coord + int2( 1, 0)));
	float3 up    = getViewPos(coord + int2( 0,-1), fetchDepth(coord + int2( 0,-1)));
	float3 down  = getViewPos(coord + int2( 0, 1), fetchDepth(coord + int2( 0, 1)));

	// Use the pair with the smaller depth difference (avoids edge artifacts)
	float3 dx = (abs(left.z - view_pos.z) < abs(right.z - view_pos.z))
		? (view_pos - left) : (right - view_pos);
	float3 dy = (abs(up.z - view_pos.z) < abs(down.z - view_pos.z))
		? (view_pos - up) : (down - view_pos);

	return normalize(cross(dx, dy));
}

// Spatial noise for slice rotation and step jitter
float spatialNoise(uint2 coord) {
	return frac(52.9829189 * frac(dot(float2(coord), float2(0.06711056, 0.00583715))));
}

static const uint NUM_SLICES = 4;
static const uint NUM_STEPS  = 4;

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
	float3 view_normal = reconstructNormal(depth_coord, view_pos);

	// Screen-space radius from world-space radius
	float ss_radius = push.proj_scale * push.radius / abs(view_pos.z);
	ss_radius = max(ss_radius, 3.0); // minimum 3 pixels
	ss_radius = min(ss_radius, min(push.ao_size.x, push.ao_size.y) * 0.5);

	float noise = spatialNoise(id.xy);
	float ao = 0.0;

	for (uint slice = 0; slice < NUM_SLICES; slice++) {
		// Evenly distributed angle + noise offset
		float phi = (GTAO_PI / float(NUM_SLICES)) * (float(slice) + noise);
		float2 dir = float2(cos(phi), sin(phi));

		float horizon_cos_pos = -1.0;
		float horizon_cos_neg = -1.0;

		for (uint step = 1; step <= NUM_STEPS; step++) {
			float t = (float(step) + noise * 0.5) / float(NUM_STEPS);
			float2 offset = dir * ss_radius * t;

			// Positive direction
			{
				int2 sample_coord = depth_coord + int2(offset * (push.depth_size / push.ao_size));
				sample_coord = clamp(sample_coord, int2(0), int2(push.depth_size) - 1);
				float sample_depth = fetchDepth(sample_coord);
				if (sample_depth > 0.0) {
					float3 sample_pos = getViewPos(sample_coord, sample_depth);
					float3 horizon_vec = sample_pos - view_pos;
					float len = length(horizon_vec);
					if (len > 0.001 && len < push.radius * 2.0) {
						float cos_h = dot(horizon_vec / len, view_normal);
						// Distance falloff
						float falloff = saturate(1.0 - len / (push.radius * 2.0));
						cos_h = lerp(-1.0, cos_h, falloff);
						horizon_cos_pos = max(horizon_cos_pos, cos_h);
					}
				}
			}

			// Negative direction
			{
				int2 sample_coord = depth_coord - int2(offset * (push.depth_size / push.ao_size));
				sample_coord = clamp(sample_coord, int2(0), int2(push.depth_size) - 1);
				float sample_depth = fetchDepth(sample_coord);
				if (sample_depth > 0.0) {
					float3 sample_pos = getViewPos(sample_coord, sample_depth);
					float3 horizon_vec = sample_pos - view_pos;
					float len = length(horizon_vec);
					if (len > 0.001 && len < push.radius * 2.0) {
						float cos_h = dot(horizon_vec / len, view_normal);
						float falloff = saturate(1.0 - len / (push.radius * 2.0));
						cos_h = lerp(-1.0, cos_h, falloff);
						horizon_cos_neg = max(horizon_cos_neg, cos_h);
					}
				}
			}
		}

		// GTAO analytical integration:
		// For each half-slice, integrate visibility = 0.25 * (-cos(2h-n) + cos(n) + 2h*sin(n))
		// where h = acos(horizon_cos), n = angle between normal and slice plane
		float n_dot_dir = dot(view_normal, float3(dir, 0.0));
		float n_angle = acos(clamp(n_dot_dir, -1.0, 1.0)) - GTAO_PI * 0.5;

		float h_pos = acos(clamp(horizon_cos_pos, -1.0, 1.0));
		float h_neg = acos(clamp(horizon_cos_neg, -1.0, 1.0));

		// Clamp horizon to normal hemisphere
		h_pos = max(h_pos, n_angle);
		h_neg = max(h_neg, -n_angle);

		// Integrate per half
		float vis_pos = 0.25 * (-cos(2.0 * h_pos - n_angle) + cos(n_angle) + 2.0 * h_pos * sin(n_angle));
		float vis_neg = 0.25 * (-cos(2.0 * h_neg + n_angle) + cos(n_angle) + 2.0 * h_neg * sin(-n_angle));

		ao += vis_pos + vis_neg;
	}

	ao /= float(NUM_SLICES);
	ao = saturate(ao);
	ao = pow(ao, push.intensity);

	ao_out[id.xy] = ao;
}
