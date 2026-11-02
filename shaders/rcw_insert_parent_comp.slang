// For every cascade-n probe, insert its nearest cascade-(n+1) probe, the one
// its rays deposit into, at the probe's own LOD. The merge reads all 8 parents
// around a probe and uses whichever of them exist.

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	uint parity = uint(push.anchor0_curr.w);
	RcwCascadeDesc desc = cascade_descs[push.cascade_index];
	uint count = counters[rcwCounterIndex(push.cascade_index, parity)].probes;

	uint key_curr = 0u;
	uint key_prev = 0u;
	if (thread_id.x < count) {
		uint2 probe_record = probes[desc.probe_base + parity * push.probes_per_parity + thread_id.x];
		if (probe_record.y != RCW_INVALID) {
			int3 offset;
			uint lod;
			rcwDecodeKey(probe_record.x, offset, lod);
			int scale = int(push.cascade_index + lod);
			int3 parent_grid = rcwNearestParent((push.anchor0_curr.xyz >> scale) + offset);
			key_curr = rcwEncodeKey(parent_grid, push.anchor0_curr.xyz >> (scale + 1), lod);
			key_prev = rcwEncodeKey(parent_grid, push.anchor0_prev.xyz >> (scale + 1), lod);
		}
	}
	rcwInsertProbeElected(push.cascade_index + 1u, parity, key_curr, key_prev);
}
