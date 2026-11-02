// Raygen step 2 for cascades below the top one.
// Every probe of push.cascade_index reserves
// its run inside its parent's segment. One thread per probe, indirect over that
// cascade's linear_x.
//
// Cascades run highest to lowest in separate dispatches.
// The idea is thatparent's rays stay one contiguous stretch of the R2 sequence,
// since the parent's own direction set is the union of theirs.
//
// Afterwards ray_cursor[probe] holds this probe's base, for its own children
// next dispatch and, at cascade 0, for the trace to bump per ray.

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	uint cascade = push.cascade_index;
	uint parity = uint(push.anchor0_curr.w);
	if (thread_id.x >= counters[rcwCounterIndex(cascade, parity)].probes)
		return;

	uint slot = cascade_descs[cascade].probe_base + thread_id.x;
	uint own = ray_cursor[slot];
	if (own == 0u)
		return;

	int3 grid;
	uint lod;
	uint parent_index;
	uint base;
	if (rcwProbeGrid(cascade, thread_id.x, grid, lod)
		&& rcwParentProbe(cascade, grid, lod, parent_index)) {
		InterlockedAdd(ray_cursor[cascade_descs[cascade + 1u].probe_base + parent_index], own, base);
	} else {
		// No parent to nest under. Taking a segment straight from the global
		// cursor keeps it disjoint from every other probe's, which is what the
		// trace's indexing requires; only the contiguity with a parent is lost.
		InterlockedAdd(counters[rcwCounterIndex(0u, parity)].raygen_cursor, own, base);
	}
	ray_cursor[slot] = base;
}
