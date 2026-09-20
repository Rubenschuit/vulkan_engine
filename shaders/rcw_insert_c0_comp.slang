// Cascade-0 probe insertion: every spawn pixel claims the 8 c0 probes around
// its surface point (spacing scaled by the pixel's distance LOD), or only the
// nearest one under RCW_DEPOSIT_NEAREST.
// One thread per RC output pixel, no early returns so the wave election in
// rcwInsertProbeElected stays uniform.

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(8, 8, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	int2 pixel = int2(thread_id.xy);
	bool valid = pixel.x < int(push.out_size.x) && pixel.y < int(push.out_size.y);
	float3 world;
	uint lod = 0u;
	float lod_blend = 0.0;
	if (valid)
		valid = rcwPixelWorld(pixel, world, lod, lod_blend);

	uint parity = uint(push.anchor0_curr.w);
	bool nearest = rcwDepositMode() == RCW_DEPOSIT_NEAREST;
	int3 cell0 = int3(0, 0, 0);
	if (valid)
		cell0 = rcwCell0(world);

	// LOD blend zone: insert at BOTH lods
	for (uint pass = 0; pass < 2; pass++) {
		uint pass_lod = lod + pass;
		bool pass_valid = valid && (pass == 0u || (lod_blend > 0.0 && pass_lod <= RCW_MAX_LOD));
		int scale = int(pass_lod); // cascade 0
		int3 anchor = push.anchor0_curr.xyz >> scale;
		int3 anchor_prev = push.anchor0_prev.xyz >> scale;
		int3 base = int3(0, 0, 0);
		int3 nearest_index = int3(0, 0, 0);
		if (pass_valid) {
			float3 position_in_cell;
			base = rcwProbeBase(world, scale, position_in_cell);
			if (nearest)
				nearest_index = rcwNearestIndex(cell0, scale);
		}
		// Fixed 8 iterations for the wave election; nearest mode emits on the first only
		for (uint corner_index = 0; corner_index < 8; corner_index++) {
			uint key_curr = 0u;
			uint key_prev = 0u;
			if (pass_valid && (!nearest || corner_index == 0u)) {
				int3 corner_cell = nearest ? nearest_index : base + rcwCorner(corner_index);
				key_curr = rcwEncodeKey(corner_cell, anchor, pass_lod);
				key_prev = rcwEncodeKey(corner_cell, anchor_prev, pass_lod);
			}
			rcwInsertProbeElected(0u, parity, key_curr, key_prev);
		}
	}
}
