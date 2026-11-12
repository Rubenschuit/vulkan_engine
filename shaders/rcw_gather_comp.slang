// For each output pixel we get the irradiance tiles of the 8 c0 probes
// around the pixel's surface point and read the texel in the
// direction of the normal, averaging them. Weights are trilinear times
// a falloff with distance from the surface plane, divided by the total
// weight of the probes that exist.
//
// Output: rgb = the averaged radiance,
// 		   a = the tiles' a, averaged.
//
// The specular lobe is read in its centroid direction at the roughness's
// lobe level (rc_spec_output rgb, and its sky estimate in
// rc_visibility_output). Rough lobes come from the same probes' tiles: the
// specular tile, mixed toward the irradiance tile as roughness widens the
// lobe past it. Narrower lobes walk the cascades: at level n,
// every cascade up to n contributes its interval (J, beta), interpolated over
// its probes and bilinearly over its bins.
//
// Sky pixels and pixels with no probes around them write a = 0, so the
// composite knows to use the environment there.

#include "rc_world_common.slangh"

// The roughness whose GGX lobe the specular tile's lobe matches in spread
// (see RCW_SPEC_TILE_SHARPNESS)
static const float RCW_SPEC_TILE_ROUGHNESS = 0.6;

// Lobe levels: the specular tile, then the walk reading its far field at
// cascade 0, 1 and 2's bins. Each level's lobe matches the GGX lobe of the
// roughness beside it in spread.
static const uint RCW_SPEC_LEVELS = 4;
static const float RCW_SPEC_LEVEL_ROUGHNESS[RCW_SPEC_LEVELS] = { RCW_SPEC_TILE_ROUGHNESS, 0.40, 0.256, 0.171 };
static const float RCW_SPEC_LEVEL_SPREAD[RCW_SPEC_LEVELS] = { 0.217, 0.0914, 0.0257, 0.00712 };
static const uint RCW_SPEC_MAX_WALK = RCW_SPEC_LEVELS - 2u;

// Fractional lobe level of a roughness: -1 is the specular tile, n >= 0 the
// walk at level n. Between two levels the GGX spread is mixed.
float rcwSpecLevel(float roughness) {
	if (roughness >= RCW_SPEC_LEVEL_ROUGHNESS[0])
		return -1.0;
	for (uint level = 0; level + 1u < RCW_SPEC_LEVELS; level++) {
		float wide_roughness = RCW_SPEC_LEVEL_ROUGHNESS[level];
		float sharp_roughness = RCW_SPEC_LEVEL_ROUGHNESS[level + 1u];
		if (roughness >= sharp_roughness) {
			float spread_ratio = RCW_SPEC_LEVEL_SPREAD[level + 1u] / RCW_SPEC_LEVEL_SPREAD[level];
			float exponent = log(spread_ratio) / log(sharp_roughness / wide_roughness);
			float sharp_share = (1.0 - pow(roughness / wide_roughness, exponent)) / (1.0 - spread_ratio);
			return float(level) - 1.0 + sharp_share;
		}
	}
	return float(RCW_SPEC_MAX_WALK);
}

// The probes of one cascade and lod around a surface point: trilinear weight
// times the falloff from the surface plane, RCW_INVALID where absent
struct RcwSpecTaps {
	uint index[8];
	float weight[8];
	float weight_sum;
};

RcwSpecTaps rcwSpecTaps(float3 world, float3 normal, uint cascade, uint lod, uint parity) {
	RcwSpecTaps taps;
	taps.weight_sum = 0.0;
	int scale = int(cascade + lod);
	float spacing = rcwSpacing(scale);
	float3 fraction;
	int3 cell = rcwProbeBase(world, scale, fraction);
	int3 anchor = push.anchor0_curr.xyz >> scale;
	RcwCascadeDesc desc = cascade_descs[cascade];
	for (uint corner_index = 0; corner_index < 8; corner_index++) {
		taps.index[corner_index] = RCW_INVALID;
		taps.weight[corner_index] = 0.0;
		float trilinear_weight = rcwCornerWeight(corner_index, fraction);
		if (trilinear_weight <= 1e-5)
			continue;
		int3 grid = cell + rcwCorner(corner_index);
		uint probe_index;
		if (!rcwHashLookup(desc, parity, rcwEncodeKey(grid, anchor, lod), probe_index))
			continue;
		float plane_distance = dot(rcwProbePosition(grid, scale) - world, normal);
		taps.index[corner_index] = probe_index;
		taps.weight[corner_index] = trilinear_weight * exp(-RCW_GATHER_PLANE_K * abs(plane_distance) / spacing);
		taps.weight_sum += taps.weight[corner_index];
	}
	return taps;
}

// One cascade's interval along a direction: J, beta, the share the merge
// counts as the bin's own and own * beta, each averaged over the taps.
struct RcwSpecInterval {
	float3 radiance;
	float beta;
	float own;
	float own_beta;
};

RcwSpecInterval rcwSpecInterval(uint cascade, uint parity, RcwSpecTaps taps, float3 direction) {
	bool always_own = cascade == 0u && push.n_cascades > 1u;
	RcwSpecInterval interval;
	interval.radiance = float3(0.0, 0.0, 0.0);
	interval.beta = 1.0;
	interval.own = always_own ? 1.0 : 0.0;
	interval.own_beta = interval.own;
	if (!(taps.weight_sum > 1e-5))
		return interval;
	uint bins[4];
	float bin_weights[4];
	rcwBinBilinear(direction, cascade, bins, bin_weights);
	RcwCascadeDesc desc = cascade_descs[cascade];
	uint direction_count = rcwDirCount(cascade);
	float4 j_beta = float4(0.0, 0.0, 0.0, 0.0);
	float own = 0.0;
	float own_beta = 0.0;
	for (uint corner_index = 0; corner_index < 8; corner_index++) {
		if (taps.index[corner_index] == RCW_INVALID)
			continue;
		float weight = taps.weight[corner_index] / taps.weight_sum;
		uint2 record = probes[desc.probe_base + parity * push.probes_per_parity + taps.index[corner_index]];
		uint slot = record.y & RCW_SLOT_MASK;
		if (record.y == RCW_INVALID || slot >= desc.probe_cap) {
			j_beta.a += weight;
			continue;
		}
		uint pair_base = desc.pair_base + slot * direction_count;
		for (uint tap = 0; tap < 4; tap++) {
			float tap_weight = weight * bin_weights[tap];
			float4 stored = rcwUnpackJBeta(persist_j[pair_base + bins[tap]]);
			j_beta += stored * tap_weight;
			if (always_own || persist_count[pair_base + bins[tap]] > 0u) {
				own += tap_weight;
				own_beta += stored.a * tap_weight;
			}
		}
	}
	interval.radiance = j_beta.rgb;
	interval.beta = j_beta.a;
	interval.own = own;
	interval.own_beta = own_beta;
	return interval;
}

// What lies beyond an interval along the direction: radiance, evidence and
// proven sky fraction.
struct RcwSpecBeyond {
	float3 radiance;
	float evidence;
	float lower;
};

// `cascade`s dirmip, read at cascade - 1's bins, with count-weighted interpolation.
// Past the top cascade lies the sky
RcwSpecBeyond rcwSpecBeyond(uint cascade, RcwSpecTaps taps, float3 direction) {
	RcwSpecBeyond beyond;
	beyond.radiance = float3(0.0, 0.0, 0.0);
	beyond.evidence = 1.0;
	beyond.lower = 1.0;
	if (cascade >= push.n_cascades)
		return beyond;
	beyond.evidence = 0.0;
	beyond.lower = 0.0;
	uint bins[4];
	float bin_weights[4];
	rcwBinBilinear(direction, cascade - 1u, bins, bin_weights);
	RcwCascadeDesc desc = cascade_descs[cascade];
	uint direction_count = rcwDirCount(cascade - 1u);
	float weight_sum = 0.0;
	for (uint corner_index = 0; corner_index < 8; corner_index++) {
		if (taps.index[corner_index] == RCW_INVALID)
			continue;
		uint pair_base = desc.dirmip_base + taps.index[corner_index] * direction_count;
		for (uint tap = 0; tap < 4; tap++) {
			uint pair = pair_base + bins[tap];
			uint2 word = dirmip[pair];
			uint bits = word.y >> 16;
			uint count = rcwUnpackCount(bits);
			if (count == 0u)
				continue;
			float weight = taps.weight[corner_index] * bin_weights[tap] * float(count);
			beyond.radiance += rcwUnpackHalf4(word).rgb * weight;
			beyond.evidence += rcwUnpackEvidence(bits) * weight;
			beyond.lower += dirmip_lower[pair] * weight;
			weight_sum += weight;
		}
	}
	if (weight_sum > 0.0) {
		beyond.radiance /= weight_sum;
		beyond.evidence /= weight_sum;
		beyond.lower /= weight_sum;
	}
	return beyond;
}

// An interval in front of what lies beyond it
// (evidence = own * ((1 - beta) + beta * E), lower = own * beta * L)
RcwSpecBeyond rcwSpecComposite(RcwSpecInterval interval, RcwSpecBeyond beyond) {
	RcwSpecBeyond result;
	result.radiance = interval.radiance + interval.beta * beyond.radiance;
	result.evidence = interval.own - interval.own_beta * (1.0 - beyond.evidence);
	result.lower = interval.own_beta * beyond.lower;
	return result;
}

// rgb = the bounce along the lobe; a = the sky estimate
float4 rcwSpecResolve(RcwSpecBeyond path, float fill) {
	float sky = min(path.lower, path.evidence) + fill * (1.0 - path.evidence);
	return float4(path.radiance, (push.flags & RCW_FLAG_VISIBILITY_EVIDENCE) != 0u ? path.evidence : sky);
}

// The walk at one lod, at level `level` mixed toward level + 1 by
// `next_share`. taps0 are the lod's cascade-0 probes; false without any.
bool rcwSpecWalk(float3 world, float3 normal, uint lod, uint parity, RcwSpecTaps taps0, float3 direction,
	uint level, float next_share, out float4 result) {
	result = float4(0.0, 0.0, 0.0, 0.0);
	if (!(taps0.weight_sum > 1e-5))
		return false;
	float fill = 0.0;
	for (uint corner_index = 0; corner_index < 8; corner_index++) {
		if (taps0.index[corner_index] != RCW_INVALID)
			fill += probe_sky_fill[taps0.index[corner_index]] * taps0.weight[corner_index];
	}
	fill /= taps0.weight_sum;

	uint deepest = next_share > 0.0 ? level + 1u : level;
	RcwSpecInterval intervals[RCW_SPEC_MAX_WALK + 1u];
	RcwSpecBeyond beyond_level = (RcwSpecBeyond)0;
	RcwSpecBeyond beyond_next = (RcwSpecBeyond)0;
	RcwSpecTaps taps = taps0;
	for (uint cascade = 0; cascade <= deepest + 1u; cascade++) {
		if (cascade > 0u && cascade < push.n_cascades)
			taps = rcwSpecTaps(world, normal, cascade, lod, parity);
		if (cascade <= deepest)
			intervals[cascade] = rcwSpecInterval(cascade, parity, taps, direction);
		if (cascade == level + 1u)
			beyond_level = rcwSpecBeyond(cascade, taps, direction);
		if (cascade == level + 2u)
			beyond_next = rcwSpecBeyond(cascade, taps, direction);
	}
	RcwSpecBeyond path = beyond_level;
	for (int cascade = int(level); cascade >= 0; cascade--)
		path = rcwSpecComposite(intervals[cascade], path);
	result = rcwSpecResolve(path, fill);
	if (next_share > 0.0) {
		path = beyond_next;
		for (int cascade = int(level) + 1; cascade >= 0; cascade--)
			path = rcwSpecComposite(intervals[cascade], path);
		result = lerp(result, rcwSpecResolve(path, fill), next_share);
	}
	return true;
}

// Centroid of the GGX specular lobe (D * G * cos over light directions).
// Fitted numerically, w is close to alpha = roughness^2 where
// the tiles are used (roughness 0.6 and up): within about
// 0.1 for NV from 0.3 to 0.9, leaning less toward N than GGX at grazing view.
float3 rcwSpecLobeDirection(float3 reflection, float3 normal, float roughness) {
	return normalize(lerp(reflection, normal, roughness * roughness));
}

// Share of the irradiance tile in the specular lobe. Mixing two lobes mixes
// their mean cosines; this fit matches the mix to the GGX lobe's to within
// 0.1 for N.V from 0.4 to 1. None up to RCW_SPEC_TILE_ROUGHNESS; 0.85 at
// roughness 1.
float rcwSpecWideShare(float roughness) {
	float x = saturate((1.0 - roughness) / (1.0 - RCW_SPEC_TILE_ROUGHNESS));
	return 0.85 * (1.0 - pow(x, 1.8));
}

// The tiles of one lod's 8 cascade-0 probes, scaled by the blend factor
// between two lods. Missing probes add nothing; the caller divides by the
// weight that was found across both lods.
void rcwGatherLod(RcwSpecTaps taps, uint tiles_per_row, float2 atlas_size, float2 oct_normal, float2 oct_lobe,
	float lod_weight, inout float4 radiance_sum, inout float4 lobe_wide_sum, inout float4 lobe_sharp_sum,
	inout float weight_sum) {
	for (uint corner_index = 0; corner_index < 8; corner_index++) {
		uint probe_index = taps.index[corner_index];
		if (probe_index == RCW_INVALID)
			continue;
		float weight = lod_weight * taps.weight[corner_index];
		float2 tile_origin = float2(probe_index % tiles_per_row, probe_index / tiles_per_row) * float(RCW_TILE);
		float2 normal_uv = (tile_origin + oct_normal) / atlas_size;
		float2 lobe_uv = (tile_origin + oct_lobe) / atlas_size;
		radiance_sum += irradiance_atlas_in.SampleLevel(linear_sampler, normal_uv, 0) * weight;
		lobe_wide_sum += irradiance_atlas_in.SampleLevel(linear_sampler, lobe_uv, 0) * weight;
		lobe_sharp_sum += spec_atlas_in.SampleLevel(linear_sampler, lobe_uv, 0) * weight;
		weight_sum += weight;
	}
}

[shader("compute")]
[numthreads(16, 16, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	int2 pixel = int2(thread_id.xy);
	if (pixel.x >= int(push.out_size.x) || pixel.y >= int(push.out_size.y))
		return;

	rc_visibility_output[pixel] = 1.0;
	rc_spec_output[pixel] = float4(0.0);

	// The surface at the full-res pixel under this texel's centre, for the
	// composite's edge-aware upsample
	int2 surface_pixel = rcwDepthTexel(pixel);
	float surface_depth = depth_texture.Load(int3(surface_pixel, 0));
	float4 surface_normal_roughness = normal_roughness_texture.Load(int3(surface_pixel, 0));
	float3 surface_normal = surface_normal_roughness.xyz;
	if (surface_depth > 0.0 && dot(surface_normal, surface_normal) >= 1e-12)
		rc_geometry_output[pixel] = float4(normalize(surface_normal), surface_depth);
	else
		rc_geometry_output[pixel] = float4(0.0);
	uint mode = push.debug_gather_mode;
	float3 world;
	uint lod;
	float lod_blend;
	bool hit = rcwPixelWorld(pixel, world, lod, lod_blend);
	if (!hit) {
		rc_output[pixel] = float4(0.0);
		return;
	}
	if (mode == RCW_GATHER_TRACE_KIND)
		return; // the trace wrote this view already
	bool debug_mode = mode == RCW_GATHER_PROBE_J || mode == RCW_GATHER_CONVERGENCE
		|| mode == RCW_GATHER_PROBE_GRID;
	uint parity = uint(push.anchor0_curr.w);
	int3 cell0 = rcwCell0(world);
	if (!debug_mode) {
		// 8 c0 probes per lod; in the blend zone both lods, mixed by lod_blend.
		// An MSAA resolve can average opposite normals to zero
		if (dot(surface_normal, surface_normal) < 1e-12) {
			rc_output[pixel] = float4(0.0);
			return;
		}
		float3 normal = normalize(surface_normal);
		uint tiles_per_row = rcwTilesPerRow(cascade_descs[0].probe_cap);
		float2 atlas_size;
		irradiance_atlas_in.GetDimensions(atlas_size.x, atlas_size.y);
		float roughness = saturate(surface_normal_roughness.w);
		float2 oct_normal = rcwOctEncode(normal) * float(RCW_TILE_INNER) + 1.0;
		float3 reflection = reflect(-normalize(ubo.camera_position.xyz - world), normal);
		float3 lobe_direction = rcwSpecLobeDirection(reflection, normal, roughness);
		bool cosine_visibility = (push.flags & RCW_FLAG_VISIBILITY_COSINE) != 0u;
		float2 oct_lobe = cosine_visibility ? oct_normal : rcwOctEncode(lobe_direction) * float(RCW_TILE_INNER) + 1.0;

		float4 radiance_sum = float4(0.0, 0.0, 0.0, 0.0);
		float4 lobe_wide_sum = float4(0.0, 0.0, 0.0, 0.0);
		float4 lobe_sharp_sum = float4(0.0, 0.0, 0.0, 0.0);
		float weight_sum = 0.0;
		RcwSpecTaps taps_lod = rcwSpecTaps(world, normal, 0u, lod, parity);
		rcwGatherLod(taps_lod, tiles_per_row, atlas_size, oct_normal, oct_lobe,
			1.0 - lod_blend, radiance_sum, lobe_wide_sum, lobe_sharp_sum, weight_sum);
		bool next_lod = lod_blend > 0.0 && lod < RCW_MAX_LOD;
		RcwSpecTaps taps_next_lod = taps_lod;
		if (next_lod) {
			taps_next_lod = rcwSpecTaps(world, normal, 0u, lod + 1u, parity);
			rcwGatherLod(taps_next_lod, tiles_per_row, atlas_size, oct_normal, oct_lobe,
				lod_blend, radiance_sum, lobe_wide_sum, lobe_sharp_sum, weight_sum);
		}
		if (!(weight_sum > 1e-5)) {
			rc_output[pixel] = float4(0.0);
			return;
		}

		// The specular lobe: the tiles, then the walk below RCW_SPEC_TILE_ROUGHNESS
		// down to where the composite starts using RC specular. The tiles carry
		// their SH sky in rgb and replace the environment. The walk's
		// rgb is bounce only, and the composite adds the environment for its
		// share, darkened by the sky estimate. Smoother surfaces read only the
		// sky estimate, and the specular tile's does.
		float4 tile = float4(lerp(lobe_sharp_sum.rgb, lobe_wide_sum.rgb, rcwSpecWideShare(roughness)),
			lobe_sharp_sum.a) / weight_sum;
		float replaces = saturate(lobe_wide_sum.a / weight_sum);
		float3 spec_radiance = tile.rgb;
		float sky_estimate = tile.a;
		bool specular = (push.flags & RCW_FLAG_NO_SPECULAR) == 0u;
		float level = min(rcwSpecLevel(roughness), float(min(push.n_cascades - 1u, RCW_SPEC_MAX_WALK)));
		float band_start = RC_SPEC_HANDOFF_ROUGHNESS * SPEC_HANDOFF_FADE_START;
		if (specular && !cosine_visibility && level > -1.0 && roughness >= band_start) {
			uint walk_level = uint(max(level, 0.0));
			float next_share = max(level, 0.0) - float(walk_level);
			// Each lod's walk weighs what its cascade-0 probes weigh in the tiles
			float4 walk = float4(0.0, 0.0, 0.0, 0.0);
			float walk_weight = 0.0;
			float4 walk_lod;
			if (lod_blend < 1.0 && rcwSpecWalk(world, normal, lod, parity, taps_lod, lobe_direction, walk_level,
					next_share, walk_lod)) {
				float lod_weight = (1.0 - lod_blend) * taps_lod.weight_sum;
				walk += walk_lod * lod_weight;
				walk_weight += lod_weight;
			}
			if (next_lod && rcwSpecWalk(world, normal, lod + 1u, parity, taps_next_lod, lobe_direction, walk_level,
					next_share, walk_lod)) {
				float lod_weight = lod_blend * taps_next_lod.weight_sum;
				walk += walk_lod * lod_weight;
				walk_weight += lod_weight;
			}
			if (walk_weight > 0.0) {
				walk /= walk_weight;
				float walk_share = saturate(level + 1.0);
				spec_radiance = lerp(tile.rgb, walk.rgb, walk_share);
				replaces *= 1.0 - walk_share;
				sky_estimate = lerp(tile.a, walk.a, smoothstep(band_start, RC_SPEC_HANDOFF_ROUGHNESS, roughness));
			}
		}
		float4 spec = specular ? float4(spec_radiance, replaces) : float4(0.0);
		if (mode == RCW_GATHER_SPEC) {
			rc_output[pixel] = float4(spec.rgb, 0.0);
			return;
		}
		radiance_sum /= weight_sum;
		rc_output[pixel] = float4(radiance_sum.rgb, saturate(radiance_sum.a));
		rc_visibility_output[pixel] = saturate(sky_estimate);
		rc_spec_output[pixel] = spec;
		return;
	}
	if (mode == RCW_GATHER_PROBE_J) {
		uint cascade = min(push.probe_j_cascade, push.n_cascades - 1u);
		int scale = int(cascade + lod);
		int3 grid = rcwNearestIndex(cell0, scale);
		uint key = rcwEncodeKey(grid, push.anchor0_curr.xyz >> scale, lod);
		RcwCascadeDesc desc = cascade_descs[cascade];
		uint probe_index;
		if (!rcwHashLookup(desc, parity, key, probe_index)) {
			rc_output[pixel] = float4(0.0);
			return;
		}
		uint slot = probes[desc.probe_base + parity * push.probes_per_parity + probe_index].y & RCW_SLOT_MASK;
		uint direction_count = rcwDirCount(cascade);
		uint pair_base = desc.pair_base + slot * direction_count;
		float3 radiance_sum = float3(0.0, 0.0, 0.0);
		uint sampled = 0u;
		for (uint bin = 0; bin < direction_count; bin++) {
			if (persist_count[pair_base + bin] == 0u)
				continue;
			radiance_sum += rcwUnpackJBeta(persist_j[pair_base + bin]).rgb;
			sampled++;
		}
		rc_output[pixel] = float4(radiance_sum / float(max(sampled, 1u)), 0.0);
		return;
	}
	if (mode == RCW_GATHER_PROBE_GRID) {
		// The pixel's nearest probe at the chosen cascade, coloured by its world
		// grid index so neighbours differ; black where the probe is absent
		uint cascade = min(push.probe_j_cascade, push.n_cascades - 1u);
		int scale = int(cascade + lod);
		int3 grid = rcwNearestIndex(cell0, scale);
		uint key = rcwEncodeKey(grid, push.anchor0_curr.xyz >> scale, lod);
		uint probe_index;
		if (!rcwHashLookup(cascade_descs[cascade], parity, key, probe_index)) {
			rc_output[pixel] = float4(0.0);
			return;
		}
		uint hash = rcwPcgHash(uint(grid.x) * 73856093u ^ uint(grid.y) * 19349663u
			^ uint(grid.z) * 83492791u ^ uint(scale) * 2654435761u);
		float3 color = float3(float(hash & 0xFFu), float((hash >> 8) & 0xFFu), float((hash >> 16) & 0xFFu)) / 255.0;
		rc_output[pixel] = float4(0.25 + 0.75 * color, 0.0);
		return;
	}
	if (mode == RCW_GATHER_CONVERGENCE) {
		RcwCascadeDesc desc = cascade_descs[0];
		uint direction_count = rcwDirCount(0);
		float convergence[2] = {0.0, 0.0};
		for (uint pass = 0; pass < 2; pass++) {
			uint lod_pass = lod + pass;
			if (lod_pass > RCW_MAX_LOD)
				continue;
			float3 fraction;
			int3 cell = rcwProbeBase(world, int(lod_pass), fraction);
			int3 anchor = push.anchor0_curr.xyz >> int(lod_pass);
			for (uint corner_index = 0; corner_index < 8; corner_index++) {
				float trilinear_weight = rcwCornerWeight(corner_index, fraction);
				uint key = rcwEncodeKey(cell + rcwCorner(corner_index), anchor, lod_pass);
				uint probe_index;
				if (!rcwHashLookup(desc, parity, key, probe_index))
					continue;
				uint slot = probes[desc.probe_base + parity * push.probes_per_parity + probe_index].y
					& RCW_SLOT_MASK;
				uint pair_base = desc.pair_base + slot * direction_count;
				uint count_sum = 0u;
				for (uint bin = 0; bin < direction_count; bin++)
					count_sum += min(persist_count[pair_base + bin], push.history_length);
				convergence[pass] += trilinear_weight * float(count_sum)
					/ (float(direction_count) * float(push.history_length));
			}
		}
		// The two lods mixed as the gather mixes them
		float value = lod < RCW_MAX_LOD ? lerp(convergence[0], convergence[1], lod_blend) : convergence[0];
		rc_output[pixel] = float4(value, value, value, 0.0);
	}
}
