// Raygen step 1.
// Count what each cascade-0 probe gets from the spawn pixels.
// One thread per RC output pixel.

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(8, 8, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	int2 pixel = int2(thread_id.xy);
	if (pixel.x >= int(push.out_size.x) || pixel.y >= int(push.out_size.y))
		return;

	float3 world;
	float3 normal;
	uint lod;
	if (!rcwSpawnPixel(pixel, world, normal, lod))
		return;

	uint probe_index;
	if (!rcwSpawnProbe0(world, lod, probe_index))
		return;

	InterlockedAdd(ray_cursor[cascade_descs[0].probe_base + probe_index], push.rays_per_pixel);
}
