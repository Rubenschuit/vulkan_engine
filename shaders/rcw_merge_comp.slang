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
// fraction travels beside it in merged_lower / dirmip_lower: sampled times
// beta at the top cascade; below, the bin's own share (sampled, or 1 at
// cascade 0) times beta times the parents' mean, 0 with no parent.
//
// An unsampled bin holds J = 0, beta = 1 from resolve, so it passes the
// parent through.
//
// Cascade 0 writes its merged bins to merged for the irradiance pass. Above
// it, the four bins that cover one bin of the cascade below sit on four
// consecutive threads (rcwBinIndex order), which average them into dirmip at
// that direction resolution: the next merge's parents. Radiance averages over
// all four, except at the top cascade, where a bin nobody sampled holds black
// and is left out; evidence and the proven-sky fraction average over all
// four, and the sample counts add up. A husk's bins merge to zero.
//
// The 8 parent lookups are the same for every bin of a probe, so each group
// resolves them once per probe it touches, one (probe, corner) per thread,
// into shared memory.

#include "rc_world_common.slangh"

// A group's RCW_LINEAR_WG consecutive pairs touch at most this many probes;
// cascade 0 has the fewest bins per probe
static const uint RCW_MERGE_GROUP_PROBES = (RCW_LINEAR_WG + RCW_C0_BINS - 2u) / RCW_C0_BINS + 1u;
static const uint RCW_MERGE_PARENT_ENTRIES = RCW_MERGE_GROUP_PROBES * 8u;
groupshared uint s_parent_probe[RCW_MERGE_PARENT_ENTRIES]; // parent probe index per (probe, corner), or RCW_INVALID

// Every thread's merged bin, staged for the 4:1 average. Sibling-major
// (rcwFoldEntry), so the averaging threads read consecutive entries.
static const uint RCW_MERGE_FOLDS = RCW_LINEAR_WG / 4u;
groupshared float4 s_fold_radiance_evidence[RCW_LINEAR_WG];
groupshared float s_fold_visibility_lower[RCW_LINEAR_WG];
groupshared uint s_fold_count[RCW_LINEAR_WG];

uint rcwFoldEntry(uint fold, uint sibling) {
	return sibling * RCW_MERGE_FOLDS + fold;
}

struct RcwMergedBin {
	float3 radiance;
	float evidence;
	float visibility_lower;
	uint count;
};

RcwMergedBin rcwZeroMergedBin() {
	RcwMergedBin result;
	result.radiance = float3(0.0, 0.0, 0.0);
	result.evidence = 0.0;
	result.visibility_lower = 0.0;
	result.count = 0u;
	return result;
}

uint2 rcwPackMerged(float3 radiance, float evidence, uint count) {
	uint2 packed = rcwPackHalf4(float4(radiance, 0.0));
	packed.y |= (rcwPackEvidence(evidence) | rcwPackCount(count)) << 16;
	return packed;
}

// One bin's interval merged with its parents, whose dirmip starts at
// parent_dirmip_base. The probe's 8 corners start at table_base in
// s_parent_probe.
RcwMergedBin rcwMergeBin(uint cascade, bool top, RcwCascadeDesc desc, uint parity, uint probe_index,
	uint bin, uint direction_count, uint table_base, uint parent_dirmip_base) {
	RcwMergedBin result = rcwZeroMergedBin();
	uint2 probe_record = probes[desc.probe_base + parity * push.probes_per_parity + probe_index];
	uint slot = probe_record.y & RCW_SLOT_MASK;
	if (probe_record.y == RCW_INVALID || slot >= desc.probe_cap)
		return result;
	uint own_pair = desc.pair_base + slot * direction_count + bin;
	float4 stored = rcwUnpackJBeta(persist_j[own_pair]);
	float beta = stored.a;
	result.radiance = stored.rgb;
	result.count = persist_count[own_pair];
	float sampled = result.count > 0u ? 1.0 : 0.0;

	if (top) {
		result.evidence = sampled;
		result.visibility_lower = sampled * beta;
	} else if (beta == 0.0) {
		// Every ray into this bin was blocked inside the interval. The parents
		// enter scaled by beta, so reading them would add nothing: the bin's own
		// opaque share is all of the evidence and none of the sky reaches it.
		result.evidence = cascade == 0u ? 1.0 : sampled;
	} else {
		int3 offset;
		uint lod;
		rcwDecodeKey(probe_record.x, offset, lod);
		int3 grid = (push.anchor0_curr.xyz >> int(cascade + lod)) + offset;
		float3 radiance_sum = float3(0.0, 0.0, 0.0);
		float evidence_sum = 0.0;
		float visibility_lower_sum = 0.0;
		float weight_sum = 0.0;
		[unroll]
		for (uint corner_index = 0; corner_index < 8; corner_index++) {
			uint parent_probe_index = s_parent_probe[table_base + corner_index];
			if (parent_probe_index == RCW_INVALID)
				continue;
			uint parent_pair = parent_dirmip_base + parent_probe_index * direction_count + bin;
			uint2 parent_word = dirmip[parent_pair];
			float parent_lower = dirmip_lower[parent_pair];
			uint parent_bits = parent_word.y >> 16;
			uint parent_count = rcwUnpackCount(parent_bits);
			if (parent_count == 0u)
				continue;
			float weight = rcwParentWeight(grid, rcwCorner(corner_index));
			weight *= float(parent_count);
			radiance_sum += weight * rcwUnpackHalf4(parent_word).rgb;
			evidence_sum += weight * rcwUnpackEvidence(parent_bits);
			visibility_lower_sum += weight * parent_lower;
			weight_sum += weight;
		}
		if (weight_sum > 0.0)
			result.radiance += beta * (radiance_sum / weight_sum);
		// An unsampled c0 bin stands for a clear interval and reports what its
		// parents know
		float own = cascade == 0u ? 1.0 : sampled;
		result.evidence = own * ((1.0 - beta) + beta * (weight_sum > 0.0 ? evidence_sum / weight_sum : 0.0));
		result.visibility_lower = own * beta * (weight_sum > 0.0 ? visibility_lower_sum / weight_sum : 0.0);
	}
	return result;
}

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 group_id: SV_GroupID, uint3 local_id: SV_GroupThreadID) {
	uint probe_pair = rcwFlatThread(group_id, local_id.x);
	uint parity = uint(push.anchor0_curr.w);
	uint cascade = push.cascade_index;
	RcwCascadeDesc desc = cascade_descs[cascade];
	uint probe_count = counters[rcwCounterIndex(cascade, parity)].probes;
	uint direction_count = rcwDirCount(cascade);
	uint pair_count = probe_count * direction_count;
	bool top = cascade == push.n_cascades - 1u;
	uint group_first_pair = rcwFlatGroup(group_id) * RCW_LINEAR_WG;
	uint group_first_probe = group_first_pair / direction_count;
	uint parent_dirmip_base = 0u;

	// No thread may return before the barriers below
	if (!top) {
		uint group_end_probe = min((group_first_pair + RCW_LINEAR_WG - 1u) / direction_count + 1u, probe_count);
		RcwCascadeDesc parent_desc = cascade_descs[cascade + 1u];
		parent_dirmip_base = parent_desc.dirmip_base;
		for (uint entry = local_id.x; entry < RCW_MERGE_PARENT_ENTRIES; entry += RCW_LINEAR_WG) {
			uint table_probe = group_first_probe + entry / 8u;
			uint parent_probe_index = RCW_INVALID;
			if (table_probe < group_end_probe) {
				uint2 record = probes[desc.probe_base + parity * push.probes_per_parity + table_probe];
				if (record.y != RCW_INVALID && (record.y & RCW_SLOT_MASK) < desc.probe_cap) {
					int3 offset;
					uint lod;
					rcwDecodeKey(record.x, offset, lod);
					int scale = int(cascade + lod);
					int3 grid = (push.anchor0_curr.xyz >> scale) + offset;
					int3 parent_anchor = push.anchor0_curr.xyz >> (scale + 1);
					uint key = rcwEncodeKey(rcwParentBase(grid) + rcwCorner(entry % 8u), parent_anchor, lod);
					uint found;
					if (rcwHashLookup(parent_desc, parity, key, found))
						parent_probe_index = found;
				}
			}
			s_parent_probe[entry] = parent_probe_index;
		}
		GroupMemoryBarrierWithGroupSync();
	}

	RcwMergedBin merged_bin = rcwZeroMergedBin();
	if (probe_pair < pair_count) {
		uint probe_index = probe_pair / direction_count;
		merged_bin = rcwMergeBin(cascade, top, desc, parity, probe_index, probe_pair % direction_count,
			direction_count, (probe_index - group_first_probe) * 8u, parent_dirmip_base);
	}

	if (cascade == 0u) {
		if (probe_pair < pair_count) {
			merged[probe_pair] = rcwPackMerged(merged_bin.radiance, merged_bin.evidence, merged_bin.count);
			merged_lower[probe_pair] = merged_bin.visibility_lower;
		}
		return;
	}

	// Groups and probes both start on a multiple of 4 pairs, so four
	// consecutive threads always hold the four siblings of one probe's bin.
	uint staged = rcwFoldEntry(local_id.x >> 2, local_id.x & 3u);
	s_fold_radiance_evidence[staged] = float4(merged_bin.radiance, merged_bin.evidence);
	s_fold_visibility_lower[staged] = merged_bin.visibility_lower;
	s_fold_count[staged] = merged_bin.count;
	GroupMemoryBarrierWithGroupSync();

	uint fold = local_id.x;
	uint coarse_pair = group_first_pair / 4u + fold;
	if (fold >= RCW_MERGE_FOLDS || coarse_pair >= pair_count / 4u)
		return;
	float3 radiance_sum = float3(0.0, 0.0, 0.0);
	uint radiance_sources = 0u;
	float evidence_sum = 0.0;
	float visibility_lower_sum = 0.0;
	uint count_sum = 0u;
	for (uint sibling = 0; sibling < 4; sibling++) {
		uint entry = rcwFoldEntry(fold, sibling);
		float4 radiance_evidence = s_fold_radiance_evidence[entry];
		uint count = s_fold_count[entry];
		if (!top || count > 0u) {
			radiance_sum += radiance_evidence.rgb;
			radiance_sources++;
		}
		evidence_sum += radiance_evidence.a;
		visibility_lower_sum += s_fold_visibility_lower[entry];
		count_sum += min(count, RCW_COUNT_MAX);
	}
	float3 radiance = radiance_sources > 0u ? radiance_sum / float(radiance_sources) : float3(0.0, 0.0, 0.0);
	dirmip[desc.dirmip_base + coarse_pair] = rcwPackMerged(radiance, evidence_sum * 0.25, count_sum);
	dirmip_lower[desc.dirmip_base + coarse_pair] = visibility_lower_sum * 0.25;
}
