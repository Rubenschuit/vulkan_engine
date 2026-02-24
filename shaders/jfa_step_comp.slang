// JFA flood step: propagates nearest-seed coordinates through the image.
// Each pixel samples its 9-tap neighborhood and keeps the coordinate of the 
// closest seed.

[vk::binding(0, 0)]
Texture2D<int2> jfa_in;
[vk::binding(1, 0)]
RWTexture2D<int2> jfa_out;

struct JfaStepPush {
	int2 image_size;
	int step_size;
	int _pad;
};
[[vk::push_constant]] JfaStepPush push;

[numthreads(16, 16, 1)]
[shader("compute")]
void compMain(uint3 dtid : SV_DispatchThreadID) {
	int2 coord = int2(dtid.xy);
	if (coord.x >= push.image_size.x || coord.y >= push.image_size.y)
		return;

	int2 best_seed = int2(-1, -1);
	float best_dist = 1e30;

	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			int2 sample_coord = coord + int2(dx, dy) * push.step_size;
			sample_coord = clamp(sample_coord, int2(0), push.image_size - 1);

			int2 seed = jfa_in.Load(int3(sample_coord, 0));
			if (seed.x < 0)
				continue;

			float2 diff = float2(coord - seed);
			float dist = dot(diff, diff);
			if (dist < best_dist) {
				best_dist = dist;
				best_seed = seed;
			}
		}
	}

	jfa_out[coord] = best_seed;
}