// SSR resolve: confidence-weighted bilateral blur of the trace output.

#include "ve_constants.slangh"

[vk::binding(0, 0)]
Texture2D<float4> ssr_input;
[vk::binding(1, 0)]
Texture2D<float> depth_texture;
[vk::binding(2, 0)]
RWTexture2D<float4> ssr_output;

struct ResolvePush {
	float2 image_size;
	float2 depth_size;
	float proj_22;
	float proj_32;
	float2 _pad;
};
[[vk::push_constant]] ResolvePush push;

static const int RADIUS = 3;
// exp(-r^2 / (2 * sigma^2)), sigma = 1.5
static const float WEIGHTS[4] = { 1.0, 0.8007, 0.4111, 0.1353 };

// Reverse-Z view-space depth
float linearViewZ(float depth) {
	return push.proj_32 / (-depth - push.proj_22);
}

float fetchViewZ(int2 coord) {
	coord = clamp(coord, int2(0), int2(push.depth_size) - 1);
	return linearViewZ(depth_texture.Load(int3(coord, 0)));
}

[numthreads(16, 16, 1)]
void compMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= uint(push.image_size.x) || id.y >= uint(push.image_size.y)) return;

	int2 coord = int2(id.xy);
	float2 res_scale = push.depth_size / push.image_size;
	float center_z = fetchViewZ(int2((float2(coord) + 0.5) * res_scale));

	float3 color_sum = float3(0.0, 0.0, 0.0);
	float color_w = 0.0;
	float conf_sum = 0.0;
	float conf_w = 0.0;

	for (int dy = -RADIUS; dy <= RADIUS; dy++) {
		for (int dx = -RADIUS; dx <= RADIUS; dx++) {
			int2 tap = clamp(coord + int2(dx, dy), int2(0), int2(push.image_size) - 1);
			float4 s = ssr_input.Load(int3(tap, 0));
			float tap_z = fetchViewZ(int2((float2(tap) + 0.5) * res_scale));

			float dz = (tap_z - center_z) / max(abs(center_z) * 0.05, 1e-3);
			float w = WEIGHTS[abs(dx)] * WEIGHTS[abs(dy)] * exp(-dz * dz);

			conf_sum += s.a * w;
			conf_w += w;
			color_sum += s.rgb * (w * s.a);
			color_w += w * s.a;
		}
	}

	float confidence = conf_sum / max(conf_w, 1e-4);
	float3 color = color_sum / max(color_w, 1e-4);
	ssr_output[id.xy] = float4(color, confidence);
}