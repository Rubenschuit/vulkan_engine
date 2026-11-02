// Raygen step 2 at the top cascade.
// Pack each top-cascade probe's reservation into the R2 sequence.
// One thread per probe of the top cascade, indirect over its linear_x.
//
// Order among top-cascade probes carries
// no meaning -- they have no parent whose segment they must stay contiguous
// inside -- so a single atomic cursor packs them just as tightly, and the
// arrival order it produces is the only thing that differs.
//
// Leaves ray_cursor[probe] holding the probe's base, which is both its own R2
// segment start and the cursor its children draw from in
// rcw_raygen_offset_down_comp.

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	uint cascade = push.n_cascades - 1u;
	uint parity = uint(push.anchor0_curr.w);
	if (thread_id.x >= counters[rcwCounterIndex(cascade, parity)].probes)
		return;

	uint slot = cascade_descs[cascade].probe_base + thread_id.x;
	uint own = ray_cursor[slot];
	if (own == 0u)
		return;

	uint base;
	InterlockedAdd(counters[rcwCounterIndex(0u, parity)].raygen_cursor, own, base);
	ray_cursor[slot] = base;
}
