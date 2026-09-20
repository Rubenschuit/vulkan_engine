// For every cascade-n probe, insert the cascade-(n+1) probes around it, at
// the probe's own LOD.
//
// Which ones, per rcwParentMode():
//   FULL      all 8 around it, the set rcw_merge_comp reads
//   NEAREST   the single nearest parent

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	uint parity = uint(push.anchor0_curr.w);
	RcwCascadeDesc desc = cascade_descs[push.cascade_index];
	uint count = counters[rcwCounterIndex(push.cascade_index, parity)].probes;
	bool valid = thread_id.x < count;

	int3 grid = int3(0, 0, 0);
	int3 anchor = int3(0, 0, 0);
	int3 anchor_prev = int3(0, 0, 0);
	uint lod = 0u;
	if (valid) {
		uint2 probe_record = probes[desc.probe_base + parity * push.probes_per_parity + thread_id.x];
		if (probe_record.y == RCW_INVALID)
			valid = false;
		else {
			int3 offset;
			rcwDecodeKey(probe_record.x, offset, lod);
			int scale = int(push.cascade_index + lod);
			grid = (push.anchor0_curr.xyz >> scale) + offset;
			anchor = push.anchor0_curr.xyz >> (scale + 1);
			anchor_prev = push.anchor0_prev.xyz >> (scale + 1);
		}
	}

	uint mode = rcwParentMode();

	// Fixed 8 iterations for wave election in rcwInsertProbeElected.
	for (uint corner_index = 0; corner_index < 8; corner_index++) {
		uint key_curr = 0u;
		uint key_prev = 0u;
		if (valid) {
			int3 corner = rcwCorner(corner_index);
			int3 cell = rcwParentBase(grid) + corner;
			bool emit = true;
			if (mode == RCW_PARENT_NEAREST) {
				emit = (corner_index == 0u);
				cell = rcwNearestParent(grid);
			}
			if (emit) {
				key_curr = rcwEncodeKey(cell, anchor, lod);
				key_prev = rcwEncodeKey(cell, anchor_prev, lod);
			}
		}
		rcwInsertProbeElected(push.cascade_index + 1u, parity, key_curr, key_prev);
	}
}
