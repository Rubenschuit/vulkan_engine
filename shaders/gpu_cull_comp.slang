// GPU frustum + prev-frame Hi-Z occlusion culling compute shader.
// Each thread tests one object against 6 frustum planes.
// When Hi-Z is enabled, objects are additionally tested against the previous frame
// hierarchical depth buffer
// Survivors atomically increment instanceCount in pre-filled draw group indirect commands
// and write instance data into the group's reserved range.
//
// Dispatch: (ceil(object_count / 256), 1, 1)

#include "ve_constants.slangh"

struct ObjectDataGPU {
	float4   bounding_sphere; // local-space center, w = radius
	uint     vertex_offset;
	uint     material_index;
	uint     material_flags;
	uint     lod_count;
	uint     lod_draw_group_id[MAX_LOD_LEVELS];
	uint     lod_index_count[MAX_LOD_LEVELS];
};

struct TransformGPU {
	float4x4 world_transform;
	float4   normal_col0;
	float4   normal_col1;
	float4   normal_col2;
};

struct DrawGroupGPU {
	uint instance_base;
	uint max_instances;
};

struct InstanceData {
	float4x4 transform;
	float4   normal_col0;
	float4   normal_col1;
	float4   normal_col2;
	uint     material_index;
	uint     lod_level;
	float    depth_offset;
	uint     material_flags;
};

struct ActiveIdEntry {
	uint gpu_id;
	uint _pad;
};

struct CullParams {
	float4   frustum_planes[6]; // xyz = normal, w = distance
	float4x4 view_proj;
	float4x4 view;
	float    p00;               // projection[0][0] (horizontal focal length)
	float    p11;               // projection[1][1] (vertical focal length)
	float    p22;               // projection[2][2] (depth mapping)
	float    p32;               // projection[3][2] (depth offset)
	uint     object_count;
	uint     hiz_enabled;       // 0 = frustum only, 1 = frustum + prev-frame Hi-Z
	uint     is_shadow_pass;
	uint     hiz_mip_count;
	float2   hiz_size;          // mip 0 resolution
	int      lod_bias;          // added to selected LOD; 0 for main pass, cascade_index for shadows
	uint     _pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<ObjectDataGPU>            objects;
[[vk::binding(1, 0)]] StructuredBuffer<TransformGPU>             transforms;
[[vk::binding(2, 0)]] ConstantBuffer<CullParams>                 cull_params;
[[vk::binding(3, 0)]] RWByteAddressBuffer                        indirect_cmds;
[[vk::binding(4, 0)]] RWStructuredBuffer<InstanceData>           instance_data;
[[vk::binding(5, 0)]] RWByteAddressBuffer                        draw_counts;

// Hi-Z bindings (only bound when hiz_enabled)
[[vk::binding(6, 0)]] Texture2D<float>      hiz_pyramid;
[[vk::binding(7, 0)]] SamplerState          hiz_sampler;

// Active object IDs (contiguous, no gaps from free-list) with draw group assignment
[[vk::binding(8, 0)]] StructuredBuffer<ActiveIdEntry> active_ids;

// Draw group metadata (instance_base, max_instances per group)
[[vk::binding(9, 0)]] StructuredBuffer<DrawGroupGPU> draw_groups;

float extractMaxScale(float4x4 m) {
	float sx = length(float3(m[0][0], m[0][1], m[0][2]));
	float sy = length(float3(m[1][0], m[1][1], m[1][2]));
	float sz = length(float3(m[2][0], m[2][1], m[2][2]));
	return max(max(sx, sy), sz);
}

bool frustumTest(float3 center, float radius) {
	for (int p = 0; p < 6; p++) {
		float dist = dot(cull_params.frustum_planes[p].xyz, center)
		           + cull_params.frustum_planes[p].w;
		if (dist < -radius)
			return false;
	}
	return true;
}

// Analytically project a world-space bounding sphere to a tight screen-space AABB in NDC.
// Computes tangent lines from the eye to the sphere in view space
bool hizOcclusionTest(float3 center_world, float radius) {
	float3 center_view = mul(cull_params.view, float4(center_world, 1.0)).xyz;
	float cx = center_view.x;
	float cy = center_view.y;
	float cz = -center_view.z; // positive forward (Vulkan: camera looks down -Z in view space)

	if (cz <= radius)
		return true; // sphere intersects or is behind near plane, assume visible

	float r2 = radius * radius;
	float cz2 = cz * cz;
	float denom = cz2 - r2; // always > 0 since cz > radius

	// X bounds: tangent from origin to circle (cx, cz) radius r in the XZ plane
	float t_xz = sqrt(cx * cx + cz2 - r2);
	float x1 = cull_params.p00 * (cx * cz + radius * t_xz) / denom;
	float x2 = cull_params.p00 * (cx * cz - radius * t_xz) / denom;

	// Y bounds: same approach
	float t_yz = sqrt(cy * cy + cz2 - r2);
	float y1 = cull_params.p11 * (cy * cz + radius * t_yz) / denom;
	float y2 = cull_params.p11 * (cy * cz - radius * t_yz) / denom;

	// Convert NDC AABB to UV [0,1]
	float2 uv_min = clamp(float2(min(x1, x2), min(y1, y2)) * 0.5 + 0.5, float2(0.0), float2(1.0));
	float2 uv_max = clamp(float2(max(x1, x2), max(y1, y2)) * 0.5 + 0.5, float2(0.0), float2(1.0));

	// Select mip level based on AABB size in pixels
	float2 aabb_pixels = (uv_max - uv_min) * cull_params.hiz_size;
	float max_dim = max(aabb_pixels.x, aabb_pixels.y);
	if (max_dim <= 0.0)
		return true; // sub-pixel, assume visible

	float mip = floor(log2(max_dim));
	mip = clamp(mip, 0.0, float(cull_params.hiz_mip_count - 1));

	// Sample Hi-Z at corners + center (take MAX = farthest depth)
	float2 uv_center = (uv_min + uv_max) * 0.5;
	float hiz_depth = hiz_pyramid.SampleLevel(hiz_sampler, uv_center, int(mip));
	float d00 = hiz_pyramid.SampleLevel(hiz_sampler, uv_min, int(mip));
	float d11 = hiz_pyramid.SampleLevel(hiz_sampler, uv_max, int(mip));
	float d01 = hiz_pyramid.SampleLevel(hiz_sampler, float2(uv_min.x, uv_max.y), int(mip));
	float d10 = hiz_pyramid.SampleLevel(hiz_sampler, float2(uv_max.x, uv_min.y), int(mip));
	hiz_depth = max(max(d00, d11), max(max(d01, d10), hiz_depth));

	// Nearest depth of the sphere: closest point along view Z axis
	// In view space z < 0 for objects in front; adding radius brings closer to 0
	float z_nearest = center_view.z + radius;
	// Project to NDC depth: clip_z = p22*z + p32, clip_w = -z (standard perspective)
	float nearest_ndc_z = (cull_params.p22 * z_nearest + cull_params.p32) / (-z_nearest);

	return nearest_ndc_z < hiz_depth;
}

// Select LOD level for an object based on its screen-space projected size.
uint selectLod(float3 center_world, float radius, uint lod_count) {
	uint lod;
	if (cull_params.is_shadow_pass != 0) {
		lod = 0;
	} else {
		float3 center_view = mul(cull_params.view, float4(center_world, 1.0)).xyz;
		float depth = max(-center_view.z, 0.001);
		float screen_frac = (radius * abs(cull_params.p11)) / depth;

		if      (screen_frac >= LOD_SCREEN_THRESHOLDS[0]) lod = 0;
		else if (screen_frac >= LOD_SCREEN_THRESHOLDS[1]) lod = 1;
		else if (screen_frac >= LOD_SCREEN_THRESHOLDS[2]) lod = 2;
		else                                               lod = 3;
	}
	int biased = int(lod) + cull_params.lod_bias;
	return uint(clamp(biased, 0, int(lod_count) - 1));
}

[numthreads(256, 1, 1)]
void compMain(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= cull_params.object_count)
		return;

	// Look up real GPU object ID from contiguous active IDs list
	uint idx = active_ids[tid.x].gpu_id;

	ObjectDataGPU obj = objects[idx];
	TransformGPU xform = transforms[idx];

	// Transform bounding sphere center to world space
	float3 center_local = obj.bounding_sphere.xyz;
	float3 center_world = mul(xform.world_transform, float4(center_local, 1.0)).xyz;
	float max_scale = extractMaxScale(xform.world_transform);
	float radius = obj.bounding_sphere.w * max_scale;

	if (!frustumTest(center_world, radius))
		return;

	// Determine bucket from material flags
	uint alpha_mode = obj.material_flags & 0x3;
	bool is_mask = (alpha_mode == 1);
	bool is_double_sided = (obj.material_flags & 0x4) != 0;
	bool is_transparent = (obj.material_flags & 0x20) != 0;

	// Transparent objects skip shadow passes
	if (is_transparent && cull_params.is_shadow_pass != 0)
		return;

	bool no_shadow = (obj.material_flags & 0x40) != 0;
	if (cull_params.is_shadow_pass != 0 && no_shadow)
		return;

	if (cull_params.hiz_enabled != 0)
		if (!hizOcclusionTest(center_world, radius))
			return;

	// Select LOD and resolve the corresponding draw group
	uint lod = selectLod(center_world, radius, obj.lod_count);

	// Stats tracking (main pass only)
	if (cull_params.is_shadow_pass == 0) {
		uint bucket;
		if (is_transparent)
			bucket = 4u + (is_double_sided ? 1u : 0u);
		else
			bucket = (is_mask ? 2u : 0u) + (is_double_sided ? 1u : 0u);
		draw_counts.InterlockedAdd(bucket * 4, 1);
		draw_counts.InterlockedAdd(GPU_CULL_BUCKET_COUNT * 4, obj.lod_index_count[lod]);
	}

	uint draw_group_id = obj.lod_draw_group_id[lod];

	// Atomically increment instanceCount in this draw group's pre-filled indirect command.
	// VkDrawIndexedIndirectCommand layout (20 bytes):
	//   offset +0: indexCount, +4: instanceCount, +8: firstIndex, +12: vertexOffset, +16: firstInstance
	uint cmd_byte_offset = draw_group_id * 20 + 4; // instanceCount field
	uint local_idx;
	indirect_cmds.InterlockedAdd(cmd_byte_offset, 1, local_idx);

	// Guard against overflow into adjacent group's instance range
	DrawGroupGPU group = draw_groups[draw_group_id];
	if (local_idx >= group.max_instances)
		return;

	uint instance_slot = group.instance_base + local_idx;

	InstanceData inst;
	inst.transform = xform.world_transform;
	inst.normal_col0 = xform.normal_col0;
	inst.normal_col1 = xform.normal_col1;
	inst.normal_col2 = xform.normal_col2;
	inst.material_index = obj.material_index;
	inst.lod_level = lod;
	inst.depth_offset = is_mask ? 0.00001 : 0.0;
	inst.material_flags = obj.material_flags;
	instance_data[instance_slot] = inst;
}