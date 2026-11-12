// One workgroup per c0 probe, one thread per texel of the probe's
// RCW_TILE^2 octahedral tiles. Each texel is one direction d and writes a
// weighted mean of the merged c0 bins I, with bin directions b, to two tiles:
//
//   irradiance  rgb = sum I * w / sum w,  w = max(dot(b, d), 0)
//               a   = 1, or 0 under RCW_SKY_MODE_OFF
//   specular    rgb = sum I * v / sum v,  v = exp(RCW_SPEC_TILE_SHARPNESS * (dot(b, d) - 1))
//               a   = the bins' sky estimate (evidence in the EVIDENCE view), weighted by v
//                     (by w in the SKY_VISIBILITY view)
//
// The irradiance tile reads d as a surface normal (a cosine lobe), the
// specular tile as a reflection direction (a narrower lobe).
//
// The irradiance rgb is the cosine-weighted mean radiance of the bounce plus the sky. The
// composite blends its own sky, times 1 - a, with rgb, so where the gather
// finds probes (a = 1) their estimate replaces the environment; the gather
// writes a = 0 where it finds none. Under RCW_SKY_MODE_OFF rgb is the bounce alone
// and a = 0, so the environment is applied as with GI off.
//
// Outside RCW_SKY_MODE_OFF, every bin gets its sky estimate times the sky
// radiance added to its radiance before the sum: the proven sky fraction, plus a fill for the
// unknown remainder. The fill is the probe's prior, or 1 or 0 under the
// UNKNOWN_SKY / UNKNOWN_OBSTRUCTED modes. The prior is the proven-open share
// of what the probe knows, scaled by how much it knows.
// Border texels use the direction on the far side of the octahedral seam, so
// the gather's bilinear tap wraps.

#include "rc_world_common.slangh"

// Every c0 bin is staged at once, one per thread, so they must fit the group
groupshared float3 s_radiance[RCW_C0_BINS];
groupshared float s_sky[RCW_C0_BINS]; // the EVIDENCE view stores evidence here instead
groupshared float3 s_direction[RCW_C0_BINS];
groupshared float2 s_prior_terms[RCW_C0_BINS]; // proven sky, evidence

[shader("compute")]
[numthreads(RCW_TILE, RCW_TILE, 1)]
void compMain(uint3 group_id: SV_GroupID, uint3 local_id: SV_GroupThreadID) {
	uint parity = uint(push.anchor0_curr.w);
	RcwCascadeDesc desc = cascade_descs[0];
	uint probe_index = rcwFlatGroup(group_id);
	uint probe_count = counters[rcwCounterIndex(0u, parity)].probes;
	if (probe_index >= probe_count)
		return;
	uint slot_word = probes[desc.probe_base + parity * push.probes_per_parity + probe_index].y;
	uint slot = slot_word & RCW_SLOT_MASK;
	if (slot_word == RCW_INVALID || slot >= desc.probe_cap)
		return;
	static_assert(RCW_C0_BINS <= RCW_TILE * RCW_TILE, "one c0 bin per thread");
	uint direction_count = rcwDirCount(0);
	uint sky_mode = rcwSkyMode();

	uint azimuth_bins = 2u * rcwThetaBins(0);
	uint texel_index = local_id.y * RCW_TILE + local_id.x;

	// Interior texels 1..6 cover [0, 1]; texels 0 and 7 fall outside and wrap
	// to the far side of the seam
	float2 oct_coord = (float2(local_id.xy) + 0.5 - 1.0) / float(RCW_TILE_INNER);
	float3 texel_direction = rcwOctDecode(rcwOctWrap(oct_coord));

	bool loader = texel_index < direction_count;
	float3 bin_radiance = float3(0.0, 0.0, 0.0);
	float evidence = 0.0;
	float proven = 0.0;
	float3 sky_radiance = float3(0.0, 0.0, 0.0);
	if (loader) {
		uint bin = texel_index;
		uint2 stored = merged[probe_index * direction_count + bin];
		float lower = merged_lower[probe_index * direction_count + bin];
		bin_radiance = rcwUnpackHalf4(stored).rgb;
		evidence = rcwUnpackEvidence(stored.y >> 16);
		// evidence is quantized to 10 bits, the proven fraction is a float: keep proven <= evidence
		proven = min(lower, evidence);
		s_prior_terms[bin] = float2(lower, evidence);
		s_direction[bin] = rcwBinToDir(uint2(bin % azimuth_bins, bin / azimuth_bins), 0);
		float3 bin_direction = s_direction[bin];
		sky_radiance = rcwSkyRadiance(bin_direction);
	}
	GroupMemoryBarrierWithGroupSync();

	if (loader) {
		// The probe's prior for unknown directions, from all of its bins
		float sky_total = 0.0;
		float known_total = 0.0;
		for (uint bin = 0; bin < direction_count; bin++) {
			float2 prior_terms = s_prior_terms[bin];
			sky_total += prior_terms.x;
			known_total += prior_terms.y;
		}
		float own_share = saturate(sky_total / max(known_total, 1e-4));
		float prior = own_share * known_total / (known_total + 1.0);
		float fill = sky_mode == RCW_SKY_MODE_UNKNOWN_SKY ? 1.0 : (sky_mode == RCW_SKY_MODE_UNKNOWN_OBSTRUCTED ? 0.0 : prior);
		if (texel_index == 0u)
			probe_sky_fill[probe_index] = fill;
		float sky = proven + fill * (1.0 - evidence);
		s_sky[texel_index] = (push.flags & RCW_FLAG_VISIBILITY_EVIDENCE) != 0u ? evidence : sky;
		s_radiance[texel_index] = sky_mode == RCW_SKY_MODE_OFF ? bin_radiance : bin_radiance + sky_radiance * sky;
	}
	GroupMemoryBarrierWithGroupSync();

	float3 radiance = float3(0.0, 0.0, 0.0);
	float weight_all = 0.0;
	float4 spec = float4(0.0, 0.0, 0.0, 0.0);
	float spec_weight_all = 0.0;
	for (uint bin = 0; bin < direction_count; bin++) {
		float cosine = dot(s_direction[bin], texel_direction);
		float spec_weight = exp(RCW_SPEC_TILE_SHARPNESS * (cosine - 1.0));
		spec_weight_all += spec_weight;
		spec += float4(s_radiance[bin], s_sky[bin]) * spec_weight;
		if (cosine <= 0.0)
			continue;
		weight_all += cosine;
		radiance += s_radiance[bin] * cosine;
	}
	float inverse_weight_all = 1.0 / max(weight_all, 1e-4);
	spec /= spec_weight_all;
	if ((push.flags & RCW_FLAG_VISIBILITY_COSINE) != 0u) {
		float sky_sum = 0.0;
		for (uint bin = 0; bin < direction_count; bin++)
			sky_sum += s_sky[bin] * max(dot(s_direction[bin], texel_direction), 0.0);
		spec.a = sky_sum * inverse_weight_all;
	}

	uint tiles_per_row = rcwTilesPerRow(desc.probe_cap);
	uint2 atlas_texel = uint2(probe_index % tiles_per_row, probe_index / tiles_per_row) * RCW_TILE + local_id.xy;
	irradiance_atlas[atlas_texel] = float4(radiance * inverse_weight_all, sky_mode == RCW_SKY_MODE_OFF ? 0.0 : 1.0);
	spec_atlas[atlas_texel] = spec;
}
