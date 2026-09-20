// Direction mip: average each 2x2 block of cascade n's merged bins down to
// cascade n-1's direction resolution

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
	uint azimuth_bins = 2u * rcwThetaBins(cascade);
	uint i = bin % azimuth_bins;
	uint j = bin / azimuth_bins;
	if ((i & 1u) != 0u || (j & 1u) != 0u)
		return;

	float3 radiance_sum = float3(0.0, 0.0, 0.0);
	for (uint dj = 0; dj < 2; dj++)
		for (uint di = 0; di < 2; di++)
			radiance_sum += rcwUnpackHalf4(merged[probe_index * direction_count + (j + dj) * azimuth_bins + (i + di)]).rgb;

	float evidence_sum = 0.0;
	float visibility_lower_sum = 0.0;
	uint count_sum = 0u;
	for (uint ej = 0; ej < 2; ej++)
		for (uint ei = 0; ei < 2; ei++) {
			uint bits = merged[probe_index * direction_count + (j + ej) * azimuth_bins + (i + ei)].y >> 16;
			evidence_sum += rcwUnpackEvidence(bits);
			count_sum += rcwUnpackCount(bits);
			visibility_lower_sum += merged_lower[probe_index * direction_count + (j + ej) * azimuth_bins + (i + ei)];
		}

	uint coarse_azimuth_bins = azimuth_bins >> 1;
	uint coarse_bin = (j >> 1) * coarse_azimuth_bins + (i >> 1);
	uint2 packed = rcwPackHalf4(float4(radiance_sum * 0.25, 0.0));
	packed.y |= (rcwPackEvidence(evidence_sum * 0.25) | rcwPackCount(count_sum)) << 16;
	dirmip[probe_index * (direction_count >> 2) + coarse_bin] = packed;
	dirmip_lower[probe_index * (direction_count >> 2) + coarse_bin] = visibility_lower_sum * 0.25;
}
