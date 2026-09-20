// For each output pixel we get the irradiance tiles of the 8 c0 probes
// around the pixel's surface point and read the texel in the
// direction of the normal, averaging them. Weights are trilinear times
// a falloff with distance from the surface plane, divided by the total
// weight of the probes that exist.
//
// Output: rgb = the averaged radiance times rc_intensity,
// 		   a = the tiles' a, averaged.
//
// Sky pixels and pixels with no probes around them write a = 0, so the
// composite knows to use the ibl there.

#include "rc_world_common.slangh"

// Weight fall off with its distance from the surface plane,
// measured in probe spacings
static const float RCW_GATHER_PLANE_K = 2.0;

// The 8 probe taps of one lod, scaled by lthe blend factor
// between two lods. Missing probes add nothing; the caller divides by the
// weight that was found across both lods.
void rcwGatherLod(float3 world, float3 normal, uint lod, uint parity, uint tiles_per_row,
	float2 atlas_size, float2 oct_normal, float2 oct_reflection, bool spec_taps, float lod_weight,
	inout float4 radiance_sum, inout float visibility_sum, inout float4 spec_sum,
	inout float weight_sum) {

	RcwCascadeDesc desc = cascade_descs[0];
	int scale = int(lod);
	float spacing = rcwSpacing(scale);
	float3 fraction;
	int3 cell = rcwProbeBase(world, scale, fraction);
	int3 anchor = push.anchor0_curr.xyz >> scale;
	for (uint corner_index = 0; corner_index < 8; corner_index++) {
		int3 corner = rcwCorner(corner_index);
		float trilinear_weight = rcwCornerWeight(corner_index, fraction);
		if (trilinear_weight <= 1e-5)
			continue;
		int3 grid = cell + corner;
		uint key = rcwEncodeKey(grid, anchor, lod);
		uint probe_index;
		if (!rcwHashLookup(desc, parity, key, probe_index))
			continue;
		float plane_distance = dot(rcwProbePosition(grid, scale) - world, normal);
		float weight = lod_weight * trilinear_weight * exp(-RCW_GATHER_PLANE_K * abs(plane_distance) / spacing);
		float2 tile_origin = float2(probe_index % tiles_per_row, probe_index / tiles_per_row) * float(RCW_TILE);
		float4 tile_sample = irradiance_atlas_in.SampleLevel(linear_sampler, (tile_origin + oct_normal) / atlas_size, 0);
		radiance_sum += tile_sample * weight;
		visibility_sum += visibility_atlas_in.SampleLevel(linear_sampler, (tile_origin + oct_normal) / atlas_size, 0).x * weight;
		if (spec_taps)
			spec_sum += irradiance_atlas_in.SampleLevel(linear_sampler, (tile_origin + oct_reflection) / atlas_size, 0) * weight;
		weight_sum += weight;
	}
}

// Rough-specular result: the irradiance tile sampled at the reflection
// direction, a cosine lobe around it.
float4 rcwSpecOutput(float4 spec_sum, float weight_sum) {
	if (!(weight_sum > 1e-5))
		return float4(0.0);
	float4 spec = spec_sum / weight_sum;
	return float4(spec.rgb, saturate(spec.a));
}

// Debug: the fraction of the 8 probes around the pixel's point that exist
// in the cascade's current map. For c0 this is 1 on geometry unless
// insertion overflowed.
// Higher cascades only insert the corners merge needs.
float rcwCornerFraction(uint cascade, uint parity, float3 world, uint lod) {
	int scale = int(cascade + lod);
	float3 position_in_cell;
	int3 cell = rcwProbeBase(world, scale, position_in_cell);
	int3 anchor = push.anchor0_curr.xyz >> scale;
	uint found = 0u;
	for (uint corner_index = 0; corner_index < 8; corner_index++) {
		int3 grid = cell + rcwCorner(corner_index);
		uint key = rcwEncodeKey(grid, anchor, lod);
		uint probe_index;
		if (rcwHashLookup(cascade_descs[cascade], parity, key, probe_index))
			found++;
	}
	return float(found) / 8.0;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	int2 pixel = int2(thread_id.xy);
	if (pixel.x >= int(push.out_size.x) || pixel.y >= int(push.out_size.y))
		return;

	rc_visibility_output[pixel] = 1.0;
	bool spec_on = (push.flags & RCW_FLAG_ROUGH_SPEC) != 0u;
	if (spec_on)
		rc_spec_output[pixel] = float4(0.0);

	// The surface at the full-res pixel under this texel's centre, for the
	// composite's edge-aware upsample
	int2 surface_pixel = rcwDepthTexel(pixel);
	float surface_depth = depth_texture.Load(int3(surface_pixel, 0));
	float3 surface_normal = normal_roughness_texture.Load(int3(surface_pixel, 0)).xyz;
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
	bool debug_mode = mode == RCW_GATHER_PROBE_J || mode == RCW_GATHER_COVERAGE
		|| mode == RCW_GATHER_CONVERGENCE || mode == RCW_GATHER_PROBE_GRID;
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
		float2 oct_normal = rcwOctEncode(normal) * float(RCW_TILE_INNER) + 1.0;
		float2 oct_reflection = float2(0.0, 0.0);
		if (spec_on) {
			float3 reflection = reflect(-normalize(ubo.camera_position.xyz - world), normal);
			oct_reflection = rcwOctEncode(reflection) * float(RCW_TILE_INNER) + 1.0;
		}

		float4 radiance_sum = float4(0.0, 0.0, 0.0, 0.0);
		float visibility_sum = 0.0;
		float4 spec_sum = float4(0.0, 0.0, 0.0, 0.0);
		float weight_sum = 0.0;
		rcwGatherLod(world, normal, lod, parity, tiles_per_row, atlas_size, oct_normal, oct_reflection, spec_on,
			1.0 - lod_blend, radiance_sum, visibility_sum, spec_sum, weight_sum);
		if (lod_blend > 0.0 && lod < RCW_MAX_LOD)
			rcwGatherLod(world, normal, lod + 1u, parity, tiles_per_row, atlas_size, oct_normal, oct_reflection, spec_on,
				lod_blend, radiance_sum, visibility_sum, spec_sum, weight_sum);
		if (mode == RCW_GATHER_SPEC) {
			rc_output[pixel] = float4(rcwSpecOutput(spec_sum, weight_sum).rgb, 0.0);
			return;
		}
		if (!(weight_sum > 1e-5)) {
			rc_output[pixel] = float4(0.0);
			return;
		}
		radiance_sum /= weight_sum;
		// Intensity scales the rgb of the irradiance
		rc_output[pixel] = float4(radiance_sum.rgb, saturate(radiance_sum.a));
		rc_visibility_output[pixel] = saturate(visibility_sum / weight_sum);
		if (spec_on)
			rc_spec_output[pixel] = rcwSpecOutput(spec_sum, weight_sum);
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
			radiance_sum += rcwUnpackHalf4(persist_j[pair_base + bin]).rgb;
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
					count_sum += min(persist_count[pair_base + bin], 64u);
				convergence[pass] += trilinear_weight * float(count_sum) / (float(direction_count) * 64.0);
			}
		}
		// The two lods mixed as the gather mixes them
		float value = lod < RCW_MAX_LOD ? lerp(convergence[0], convergence[1], lod_blend) : convergence[0];
		rc_output[pixel] = float4(value, value, value, 0.0);
		return;
	}
	float3 fractions = float3(0.0, 0.0, 0.0);
	for (uint cascade = 0; cascade < min(3u, push.n_cascades); cascade++)
		fractions[cascade] = rcwCornerFraction(cascade, parity, world, lod);
	rc_output[pixel] = float4(fractions, 0.0);
}
