// Cascade-0 probe insertion: every surface pixel claims the c0 probe nearest
// its surface point (spacing scaled by the pixel's distance LOD), at both lods
// inside the blend zone. That includes the pixels rcwSpawnPixel turns away,
// so the gather finds probes there too.
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
	int3 cell0 = int3(0, 0, 0);
	if (valid)
		cell0 = rcwCell0(world);

	// LOD blend zone: insert at BOTH lods
	for (uint pass = 0; pass < 2; pass++) {
		uint pass_lod = lod + pass;
		bool pass_valid = valid && (pass == 0u || (lod_blend > 0.0 && pass_lod <= RCW_MAX_LOD));
		uint key_curr = 0u;
		uint key_prev = 0u;
		if (pass_valid) {
			int scale = int(pass_lod); // cascade 0
			int3 nearest = rcwNearestIndex(cell0, scale);
			key_curr = rcwEncodeKey(nearest, push.anchor0_curr.xyz >> scale, pass_lod);
			key_prev = rcwEncodeKey(nearest, push.anchor0_prev.xyz >> scale, pass_lod);
		}
		rcwInsertProbeElected(0u, parity, key_curr, key_prev);
	}
}
