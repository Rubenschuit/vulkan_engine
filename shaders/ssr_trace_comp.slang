// Screen-space reflections trace.
// Marches the current-frame depth buffer along the reflection ray built from
// the prepass G-buffer normal, then fetches radiance from the previous frame's
// HDR history, reprojected via ubo.prev_projection_view (camera-only, so
// moving objects ghost slightly). Output: rgb radiance + confidence in alpha;
// confidence 0 falls back to the IBL cubemap in the composite.

#include "ve_common.slangh"

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

struct SsrPush {
	float2 out_size;
	float2 depth_size;
	float proj_22;
	float proj_32;
	float thickness;      // view-space hit tolerance
	float max_roughness;  // no reflections above this roughness
	float max_distance;   // world-space ray length
	int max_steps;
	float2 _pad;
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

	float3 world_pos = worldFromUvDepth(uv, depth);
	float3 normal = normalize(nr.xyz);
	float3 view_dir = normalize(world_pos - ubo.camera_position.xyz);
	float3 ray_dir = reflect(view_dir, normal);

	float step_len = push.max_distance / float(push.max_steps);
	float jitter = spatialNoise(id.xy);

	float t = step_len * (0.5 + jitter);
	float t_prev = 0.0;
	float hit_t = -1.0;

	for (int i = 0; i < push.max_steps; i++) {
		float3 p = world_pos + ray_dir * t;
		float4 clip = mul(ubo.projection_view, float4(p, 1.0));
		if (clip.w <= 0.0)
			break;
		float3 ndc = clip.xyz / clip.w;
		float2 p_uv = ndc.xy * 0.5 + 0.5;
		if (any(p_uv < 0.0) || any(p_uv >= 1.0))
			break;

		float scene_depth = depth_texture.Load(int3(depthCoord(p_uv), 0));
		if (scene_depth > 0.0) {
			float scene_z = linearViewZ(scene_depth);
			float ray_z = linearViewZ(ndc.z);
			// Ray went behind the surface within tolerance
			if (ray_z < scene_z && (scene_z - ray_z) < push.thickness) {
				// Binary refinement between the last miss and this hit
				float lo = t_prev;
				float hi = t;
				for (int r = 0; r < 4; r++) {
					float mid = 0.5 * (lo + hi);
					float3 pm = world_pos + ray_dir * mid;
					float4 cm = mul(ubo.projection_view, float4(pm, 1.0));
					float3 nm = cm.xyz / cm.w;
					float2 um = nm.xy * 0.5 + 0.5;
					float sd = depth_texture.Load(int3(depthCoord(um), 0));
					if (sd > 0.0 && linearViewZ(nm.z) < linearViewZ(sd))
						hi = mid;
					else
						lo = mid;
				}
				hit_t = hi;
				break;
			}
		}
		t_prev = t;
		t += step_len;
	}

	if (hit_t < 0.0) {
		ssr_out[id.xy] = float4(0, 0, 0, 0);
		return;
	}

	float3 hit_point = world_pos + ray_dir * hit_t;
	float4 hit_clip = mul(ubo.projection_view, float4(hit_point, 1.0));
	float2 hit_uv = (hit_clip.xy / hit_clip.w) * 0.5 + 0.5;
	float hit_depth = depth_texture.Load(int3(depthCoord(saturate(hit_uv)), 0));
	if (hit_depth <= 0.0 || (linearViewZ(hit_depth) - linearViewZ(hit_clip.z / hit_clip.w)) >= push.thickness) {
		ssr_out[id.xy] = float4(0, 0, 0, 0);
		return;
	}

	// Fetch last frame's radiance at the actual surface point (camera-only reprojection)
	float3 hit_world = worldFromUvDepth(hit_uv, hit_depth);
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
	float3 color = history_texture.SampleLevel(history_sampler, prev_uv, 0).rgb;

	float2 border = saturate((0.5 - abs(hit_uv - 0.5)) / 0.1);
	float edge_fade = border.x * border.y;
	float2 prev_border = saturate((0.5 - abs(prev_uv - 0.5)) / 0.1);
	float history_fade = prev_border.x * prev_border.y;
	float dist_fade = 1.0 - saturate(hit_t / push.max_distance);
	float rough_fade = 1.0 - smoothstep(push.max_roughness * 0.7, max(push.max_roughness, 1e-4), roughness);
	float confidence = edge_fade * history_fade * dist_fade * rough_fade;

	ssr_out[id.xy] = float4(color, confidence);
}
