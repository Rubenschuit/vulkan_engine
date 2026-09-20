// Cross-lod seeding of fresh probes. A probe popped fresh from the pool this
// frame starts with an empty payload, so wherever the camera-following LOD
// blend zone sweeps into new territory, the incoming lod population disagrees
// with the converged resident one until its EMA fills: a visible seam while
// the camera moves.
//
// This pass copies the persistent payload of the same-cascade probe at the
// nearby world position in a neighbouring lod: the donor is a quarter of the
// probe's own spacing away for the finer lod, half of it for the coarser.
//
// The donor's interval cutoffs differ by the lod spacing factor, so per-bin
// radiance is attributed to the wrong distance range until resplitting.
//
// Runs after keepalive but before resolve.

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	uint parity = uint(push.anchor0_curr.w);
	uint cascade = push.cascade_index;
	RcwCascadeDesc desc = cascade_descs[cascade];
	uint parity_probe_base = desc.probe_base + parity * push.probes_per_parity;
	uint count = counters[rcwCounterIndex(cascade, parity)].probes;
	if (thread_id.x >= count)
		return;
	uint2 probe_record = probes[parity_probe_base + thread_id.x];
	if (probe_record.y == RCW_INVALID)
		return;
	if ((probe_record.y & RCW_SLOT_FRESH) == 0u)
		return;
	if ((probe_record.y & RCW_SLOT_MASK) >= desc.probe_cap)
		return;

	int3 offset;
	uint lod;
	rcwDecodeKey(probe_record.x, offset, lod);
	int scale = int(cascade + lod);
	int3 cell = (push.anchor0_curr.xyz >> scale) + offset;

	// Finer lod first, then coarser.
	uint donor_slot = RCW_INVALID;
	for (uint candidate = 0; candidate < 2 && donor_slot == RCW_INVALID; candidate++) {
		uint donor_lod;
		int3 donor_cell;
		if (candidate == 0u) {
			if (lod == 0u)
				continue;
			donor_lod = lod - 1u;
			donor_cell = cell * 2;
		} else {
			if (lod + 1u > RCW_MAX_LOD)
				continue;
			donor_lod = lod + 1u;
			donor_cell = cell >> 1;
		}
		uint key = rcwEncodeKey(donor_cell, push.anchor0_curr.xyz >> int(cascade + donor_lod), donor_lod);
		uint donor_probe_index;
		if (!rcwHashLookup(desc, parity, key, donor_probe_index))
			continue;
		uint donor_slot_word = probes[parity_probe_base + donor_probe_index].y;
		if ((donor_slot_word & RCW_SLOT_FRESH) != 0u)
			continue;
		donor_slot = donor_slot_word & RCW_SLOT_MASK;
	}
	if (donor_slot == RCW_INVALID)
		return;

	uint direction_count = rcwDirCount(cascade);
	uint own_pair_base = desc.pair_base + (probe_record.y & RCW_SLOT_MASK) * direction_count;
	uint donor_pair_base = desc.pair_base + donor_slot * direction_count;
	for (uint bin = 0; bin < direction_count; bin++) {
		persist_j[own_pair_base + bin] = persist_j[donor_pair_base + bin];
		// The donor's payload is attributed to the wrong distance intervals.
		// We cap the inherited weight.
		persist_count[own_pair_base + bin] = min(persist_count[donor_pair_base + bin], 4u);
	}
	probes[parity_probe_base + thread_id.x].y = probe_record.y | RCW_SLOT_SEEDED;
}
