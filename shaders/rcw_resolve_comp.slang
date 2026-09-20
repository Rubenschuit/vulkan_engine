// Resolve: add this frame's deposits into the persistent store. One thread
// per (probe, direction) pair of push.cascade_index, indirect over
// counters[].pair_x. Scratch is indexed by probe index, the persistent store by
// slot; probes[].y links the two. Each pair has a single writer, so the
// update needs no atomics and the EMA is deterministic.
//
// Per bin: J = radiance sum / total mass, beta = clear mass / total mass (a
// clear is a sample with J = 0, beta = 1), blended into the store with
// alpha = 1 / min(sample_count + 1, 64); a sample is a frame with mass. A bin
// without mass keeps its history. Unsampled bins of a fresh slot are set to
// {0,0,0,1} / count 0, which merge reads as "pass the parent through"; a
// seeded slot already holds donor history. Under RCW_FLAG_SINGLE_FRAME every
// bin behaves like a fresh one, so the store shows one frame of rays with no
// accumulation. The scratch words consumed here
// are re-zeroed, so scratch stays zero outside the trace-to-resolve window
// without a per-frame fill.

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 group_id: SV_GroupID, uint3 local_id: SV_GroupThreadID) {
	uint probe_pair = rcwFlatThread(group_id, local_id.x);
	uint parity = uint(push.anchor0_curr.w);
	RcwCascadeDesc desc = cascade_descs[push.cascade_index];
	uint probe_count = counters[rcwCounterIndex(push.cascade_index, parity)].probes;
	uint direction_count = rcwDirCount(push.cascade_index);
	if (probe_pair >= probe_count * direction_count)
		return;
	uint probe_index = probe_pair / direction_count;
	uint bin = probe_pair % direction_count;

	uint scratch_base = (desc.pair_base + probe_pair) * RCW_SCRATCH_WORDS;
	uint radiance_r = scratch[scratch_base + 0];
	uint radiance_g = scratch[scratch_base + 1];
	uint radiance_b = scratch[scratch_base + 2];
	uint hit_mass = scratch[scratch_base + 3];
	uint clear_mass = scratch[scratch_base + 4];
	uint total_mass = hit_mass + clear_mass;

	uint slot_word = probes[desc.probe_base + parity * push.probes_per_parity + probe_index].y;
	if (slot_word == RCW_INVALID)
		return;
	uint slot = slot_word & RCW_SLOT_MASK;
	if (slot >= desc.probe_cap)
		return;
	// A seeded fresh slot carries a neighbouring-lod donor payload
	bool fresh = (slot_word & RCW_SLOT_FRESH) != 0u && (slot_word & RCW_SLOT_SEEDED) == 0u;
	uint slot_pair = desc.pair_base + slot * direction_count + bin;

	bool single_frame = (push.flags & RCW_FLAG_SINGLE_FRAME) != 0u;
	if (total_mass == 0u) {
		if (fresh || single_frame) {
			persist_j[slot_pair] = rcwPackHalf4(float4(0.0, 0.0, 0.0, 1.0));
			persist_count[slot_pair] = 0u;
		}
		return; // scratch already 0
	}

	scratch[scratch_base + 0] = 0u;
	scratch[scratch_base + 1] = 0u;
	scratch[scratch_base + 2] = 0u;
	scratch[scratch_base + 3] = 0u;
	scratch[scratch_base + 4] = 0u;

	float inverse_total = 1.0 / float(total_mass);
	float3 j_frame = float3(float(radiance_r), float(radiance_g), float(radiance_b)) * inverse_total;
	float beta_frame = float(clear_mass) * inverse_total;

	uint sample_count = (fresh || single_frame) ? 0u : persist_count[slot_pair];
	float alpha = 1.0 / float(min(sample_count + 1u, 64u));
	float4 history = float4(0.0, 0.0, 0.0, 1.0);
	if (sample_count > 0u)
		history = rcwUnpackHalf4(persist_j[slot_pair]);
	persist_j[slot_pair] = rcwPackHalf4(lerp(history, float4(j_frame, beta_frame), alpha));
	persist_count[slot_pair] = min(sample_count + 1u, 64u);
}
