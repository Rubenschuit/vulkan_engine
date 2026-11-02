// Raygen step 1 continued.
// Propagate ray counts up one cascade, so every
// probe knows how many rays land anywhere beneath it. One thread per probe of
// push.cascade_index, indirect over that cascade's linear_x.
//
// Cascades run lowest to highest in separate dispatches.

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	uint cascade = push.cascade_index;
	uint parity = uint(push.anchor0_curr.w);
	if (thread_id.x >= counters[rcwCounterIndex(cascade, parity)].probes)
		return;

	uint own = ray_cursor[cascade_descs[cascade].probe_base + thread_id.x];
	if (own == 0u)
		return;

	int3 grid;
	uint lod;
	if (!rcwProbeGrid(cascade, thread_id.x, grid, lod))
		return;

	uint parent_index;
	if (!rcwParentProbe(cascade, grid, lod, parent_index))
		return; // no parent to nest under; the offset pass falls back to the global cursor

	InterlockedAdd(ray_cursor[cascade_descs[cascade + 1u].probe_base + parent_index], own);
}
