// From the top cascade down, one thread per (probe, direction)
// pair. Each bin combines its own interval with the parent cascade:
// radiance = J + beta * parent. The parent is read from the next cascade's
// dirmip at the corners of the parent cell around this probe, with
// trilinear weights times each corner's sample count in this bin, over the
// corners that exist (a found parent nobody deposited into would pass its
// own parents through, or read black at the top cascade; a barely fed one
// counts for little). A probe sits a quarter or three quarters of the way
// between its parents on each axis, so all 8 corners get weight. With no
// parent found the radiance stays J.
//
// Evidence travels the same way, packed as 10 bits in the last half of the
// merged value beside the bin's sample count: 1 for a sampled top bin; below, the bin's own opaque share
// (1 - beta) plus beta times the parent evidence. An unsampled bin has no
// evidence, except at cascade 0, where it takes its parents'. The proven-sky
// fraction travels beside it in merged_lower: sampled times beta at the top
// cascade; below, the bin's own share (sampled, or 1 at cascade 0) times
// beta times the parents' mean, 0 with no parent.
//
// An unsampled bin holds J = 0, beta = 1 from resolve, so it passes the
// parent through. The merged buffer is reused per cascade; dirmip reads it
// before the next merge overwrites it.

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 group_id: SV_GroupID, uint3 local_id: SV_GroupThreadID) {
	uint probe_pair = rcwFlatThread(group_id, local_id.x);
	uint parity = uint(push.anchor0_curr.w);
	uint cascade = push.cascade_index;
	RcwCascadeDesc desc = cascade_descs[cascade];
	uint probe_count = counters[rcwCounterIndex(cascade, parity)].probes;
	uint direction_count = rcwDirCount(cascade);
	if (probe_pair >= probe_count * direction_count)
		return;
	uint probe_index = probe_pair / direction_count;
	uint bin = probe_pair % direction_count;

	uint2 probe_record = probes[desc.probe_base + parity * push.probes_per_parity + probe_index];
	uint slot = probe_record.y & RCW_SLOT_MASK;
	if (probe_record.y == RCW_INVALID || slot >= desc.probe_cap) {
		// Husk: write zero so dirmip reads a defined value
		merged[probe_pair] = rcwPackHalf4(float4(0.0, 0.0, 0.0, 0.0));
		merged_lower[probe_pair] = 0.0;
		return;
	}
	uint own_pair = desc.pair_base + slot * direction_count + bin;
	float4 stored = rcwUnpackHalf4(persist_j[own_pair]);
	float3 radiance = stored.rgb;
	float beta = stored.a;
	uint count = persist_count[own_pair];
	float sampled = count > 0u ? 1.0 : 0.0;
	float evidence = 0.0;
	float visibility_lower = 0.0;

	if (cascade == push.n_cascades - 1u) {
		evidence = sampled;
		visibility_lower = sampled * beta;
	} else if (beta == 0.0) {
		// Every ray into this bin was blocked inside the interval. The parents
		// enter scaled by beta, so reading them would add nothing: the bin's own
		// opaque share is all of the evidence and none of the sky reaches it.
		evidence = cascade == 0u ? 1.0 : sampled;
	} else {
		int3 offset;
		uint lod;
		rcwDecodeKey(probe_record.x, offset, lod);
		int scale = int(cascade + lod);
		int3 grid = (push.anchor0_curr.xyz >> scale) + offset;
		int3 parent_cell = rcwParentBase(grid);
		int3 parent_anchor = push.anchor0_curr.xyz >> (scale + 1);
		RcwCascadeDesc parent_desc = cascade_descs[cascade + 1u];
		float3 radiance_sum = float3(0.0, 0.0, 0.0);
		float evidence_sum = 0.0;
		float visibility_lower_sum = 0.0;
		float weight_sum = 0.0;
		for (uint corner_index = 0; corner_index < 8; corner_index++) {
			int3 corner = rcwCorner(corner_index);
			float weight = rcwParentWeight(grid, corner);
			uint key = rcwEncodeKey(parent_cell + corner, parent_anchor, lod);
			uint parent_probe_index;
			if (!rcwHashLookup(parent_desc, parity, key, parent_probe_index))
				continue;
			uint2 parent_word = dirmip[parent_probe_index * direction_count + bin];
			uint parent_bits = parent_word.y >> 16;
			uint parent_count = rcwUnpackCount(parent_bits);
			if (parent_count == 0u)
				continue;
			if ((push.flags & RCW_FLAG_COUNT_WEIGHT) != 0u)
				weight *= float(parent_count);
			radiance_sum += weight * rcwUnpackHalf4(parent_word).rgb;
			evidence_sum += weight * rcwUnpackEvidence(parent_bits);
			visibility_lower_sum += weight * dirmip_lower[parent_probe_index * direction_count + bin];
			weight_sum += weight;
		}
		if (weight_sum > 0.0)
			radiance += beta * (radiance_sum / weight_sum);
		// An unsampled c0 bin stands for a clear interval and reports what its
		// parents know
		float own = cascade == 0u ? 1.0 : sampled;
		evidence = own * ((1.0 - beta) + beta * (weight_sum > 0.0 ? evidence_sum / weight_sum : 0.0));
		visibility_lower = own * beta * (weight_sum > 0.0 ? visibility_lower_sum / weight_sum : 0.0);
	}

	uint2 packed = rcwPackHalf4(float4(radiance, 0.0));
	packed.y |= (rcwPackEvidence(evidence) | rcwPackCount(count)) << 16;
	merged[probe_pair] = packed;
	merged_lower[probe_pair] = visibility_lower;
}