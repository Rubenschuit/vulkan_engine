// One workgroup per c0 probe, one thread per texel of the probe's
// RCW_TILE^2 octahedral tile. Each texel is one direction. Treating
// that direction as a surface normal, the texel sums the merged c0 bins
//
//   rgb = sum_all I * w / sum_all w
//   a   = the Sky Visibility trust (rcwSkyTrust)
//
// with w = max(dot(bin direction, texel direction), 0).
//
// rgb is the cosine-weighted mean radiance of the bounce plus the sky, the
// sky scaled by the trust. a is that same trust: the composite blends its
// own sky, times 1 - a, with rgb, so trust 0 is the environment everywhere
// with bounce only and trust 1 the probes' estimate. The gather writes
// a = 0 where it finds no probes.
//
// Every bin gets its sky estimate times the sky radiance added to its
// radiance before the sum: the proven sky fraction, plus the probe's prior
// for the unknown remainder. The prior is the proven-open share of what the
// probe knows, scaled by how much it knows (nothing known = nothing assumed).
// Border texels use the direction on the far side of the octahedral seam, so
// the gather's bilinear tap wraps.

#include "rc_world_common.slangh"

static const uint RCW_BIN_CHUNK = 32;
groupshared float3 s_radiance[RCW_BIN_CHUNK];
groupshared float s_sky[RCW_BIN_CHUNK];
groupshared float s_evidence[RCW_BIN_CHUNK];
groupshared float3 s_direction[RCW_BIN_CHUNK];
groupshared float s_prior_sums[2]; // proven-sky total, evidence total over all bins

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
	uint direction_count = rcwDirCount(0);

	uint azimuth_bins = 2u * rcwThetaBins(0);
	uint texel_index = local_id.y * RCW_TILE + local_id.x;

	// Interior texels 1..6 cover [0, 1]; texels 0 and 7 fall outside and wrap
	// to the far side of the seam
	float2 oct_coord = (float2(local_id.xy) + 0.5 - 1.0) / float(RCW_TILE_INNER);
	float3 texel_direction = rcwOctDecode(rcwOctWrap(oct_coord));

	// The probe's prior for unknown directions, from all of its bins
	if (texel_index == 0u) {
		float sky_total = 0.0;
		float known_total = 0.0;
		for (uint bin = 0; bin < direction_count; bin++) {
			sky_total += merged_lower[probe_index * direction_count + bin];
			known_total += rcwUnpackEvidence(merged[probe_index * direction_count + bin].y >> 16);
		}
		s_prior_sums[0] = sky_total;
		s_prior_sums[1] = known_total;
	}
	GroupMemoryBarrierWithGroupSync();
	float known_total = s_prior_sums[1];
	float own_share = saturate(s_prior_sums[0] / max(known_total, 1e-4));
	float prior = own_share * known_total / (known_total + 1.0);
	float sky_trust = rcwSkyTrust();

	float3 radiance = float3(0.0, 0.0, 0.0);
	float sky_visibility = 0.0;
	float weight_all = 0.0;
	float weight_evidence = 0.0;
	for (uint chunk_start = 0; chunk_start < direction_count; chunk_start += RCW_BIN_CHUNK) {
		uint chunk_size = min(RCW_BIN_CHUNK, direction_count - chunk_start);
		if (texel_index < chunk_size) {
			uint bin = chunk_start + texel_index;
			s_radiance[texel_index] = rcwUnpackHalf4(merged[probe_index * direction_count + bin]).rgb;
			float evidence = rcwUnpackEvidence(merged[probe_index * direction_count + bin].y >> 16);
			s_evidence[texel_index] = evidence;
			// evidence is stored in 16 bits, the proven fraction in 32: keep proven <= evidence
			float proven = min(merged_lower[probe_index * direction_count + bin], evidence);
			s_sky[texel_index] = proven + prior * (1.0 - evidence);
			s_direction[texel_index] = rcwBinToDir(uint2(bin % azimuth_bins, bin / azimuth_bins), 0);
			float3 bin_direction = s_direction[texel_index];
			float3 sky_radiance = ubo.ibl_active != 0u
				? rcwEvaluateSkyRadiance(float3(bin_direction.x, bin_direction.z, bin_direction.y), ubo.sh_coefficients)
					* ubo.sky_radiance_scale.rgb
				: ubo.ambient_light_color.xyz;
			s_radiance[texel_index] += sky_radiance * s_sky[texel_index] * sky_trust;
		}
		GroupMemoryBarrierWithGroupSync();

		for (uint local_bin = 0; local_bin < chunk_size; local_bin++) {
			float weight = max(dot(s_direction[local_bin], texel_direction), 0.0);
			if (weight <= 0.0)
				continue;
			weight_all += weight;
			radiance += s_radiance[local_bin] * weight;
			sky_visibility += s_sky[local_bin] * weight;
			weight_evidence += s_evidence[local_bin] * weight;
		}
		// The next chunk overwrites the arrays.
		if (chunk_start + RCW_BIN_CHUNK < direction_count)
			GroupMemoryBarrierWithGroupSync();
	}
	float inverse_weight_all = 1.0 / max(weight_all, 1e-4);

	uint tiles_per_row = rcwTilesPerRow(desc.probe_cap);
	uint2 atlas_texel = uint2(probe_index % tiles_per_row, probe_index / tiles_per_row) * RCW_TILE + local_id.xy;
	irradiance_atlas[atlas_texel] = float4(radiance * inverse_weight_all, sky_trust);

	float visibility_out = sky_visibility * inverse_weight_all;
	// Evidence view: vis atlas holds evidence instead.
	if ((push.flags & RCW_FLAG_VISIBILITY_EVIDENCE) != 0u)
		visibility_out = weight_evidence * inverse_weight_all;
	visibility_atlas[atlas_texel] = visibility_out;
}
