// Bilateral blur for GTAO — depth-aware separable H/V.
// Dispatched twice: once horizontal (direction=1,0), once vertical (direction=0,1).
// Preserves edges using depth-based weights to avoid blurring across geometry boundaries.

#include "ve_constants.slangh"

// Set 0: Blur I/O (no global UBO needed)
[vk::binding(0, 0)]
Texture2D<float> depth_texture;
[vk::binding(1, 0)]
Texture2D<float> ao_input;
[vk::binding(2, 0)]
RWTexture2D<float> ao_output;

struct BlurPush {
	float2 image_size;
	float2 depth_size;
	int direction_x;
	int direction_y;
	float sharpness;
	float _pad;
};
[[vk::push_constant]] BlurPush push;

static const int BLUR_RADIUS = 4;

// Gaussian weights for radius 4 (sigma ~2.0)
static const float WEIGHTS[5] = {
	0.2270270270,  // center
	0.1945945946,  // offset 1
	0.1216216216,  // offset 2
	0.0540540541,  // offset 3
	0.0162162162,  // offset 4
};

float fetchDepthLinear(int2 coord) {
	coord = clamp(coord, int2(0), int2(push.depth_size) - 1);
	return depth_texture.Load(int3(coord, 0));
}

[numthreads(16, 16, 1)]
void compMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= uint(push.image_size.x) || id.y >= uint(push.image_size.y)) return;

	int2 coord = int2(id.xy);
	int2 dir = int2(push.direction_x, push.direction_y);

	// Map AO coord to depth coord (for half-res support)
	float2 uv = (float2(coord) + 0.5) / push.image_size;
	int2 depth_coord = int2(uv * push.depth_size);

	float center_ao = ao_input.Load(int3(coord, 0));
	float center_depth = fetchDepthLinear(depth_coord);

	// Skip sky pixels
	if (center_depth >= 1.0) {
		ao_output[id.xy] = 1.0;
		return;
	}

	float total_ao = center_ao * WEIGHTS[0];
	float total_weight = WEIGHTS[0];

	for (int i = 1; i <= BLUR_RADIUS; i++) {
		float w = WEIGHTS[i];

		// Positive direction
		{
			int2 sample_coord = coord + dir * i;
			int2 sample_depth_coord = depth_coord + dir * i;
			sample_coord = clamp(sample_coord, int2(0), int2(push.image_size) - 1);
			float sample_ao = ao_input.Load(int3(sample_coord, 0));
			float sample_depth = fetchDepthLinear(sample_depth_coord);

			// Depth-aware weight: reduce contribution when depth difference is large
			static const float DEPTH_WEIGHT_SCALE = 10000.0; // Scales NDC depth range [0,1]
			float depth_diff = abs(center_depth - sample_depth);
			float depth_weight = exp(-depth_diff * depth_diff * push.sharpness * DEPTH_WEIGHT_SCALE);
			float final_w = w * depth_weight;

			total_ao += sample_ao * final_w;
			total_weight += final_w;
		}

		// Negative direction
		{
			int2 sample_coord = coord - dir * i;
			int2 sample_depth_coord = depth_coord - dir * i;
			sample_coord = clamp(sample_coord, int2(0), int2(push.image_size) - 1);
			float sample_ao = ao_input.Load(int3(sample_coord, 0));
			float sample_depth = fetchDepthLinear(sample_depth_coord);

			float depth_diff = abs(center_depth - sample_depth);
			float depth_weight = exp(-depth_diff * depth_diff * push.sharpness * 10000.0);
			float final_w = w * depth_weight;

			total_ao += sample_ao * final_w;
			total_weight += final_w;
		}
	}

	ao_output[id.xy] = total_ao / max(total_weight, 0.001);
}
