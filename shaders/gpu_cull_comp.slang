// GPU frustum + prev-frame Hi-Z occlusion culling compute shader.
// Each thread tests one object against 6 frustum planes.
// When Hi-Z is enabled, objects are additionally tested against the previous frame
// hierarchical depth buffer.
// Survivors atomically increment instanceCount in pre-filled draw group indirect commands
// and write instance data into the group's reserved range.
//
// Dispatch: (ceil(object_count / 256), 1, 1)

#include "ve_constants.slangh"
#include "cull_common.slangh"

[[vk::binding(0, 0)]] StructuredBuffer<ObjectDataGPU>            objects;
[[vk::binding(1, 0)]] StructuredBuffer<TransformGPU>             transforms;
[[vk::binding(2, 0)]] ConstantBuffer<CullParams>                 cull_params;
[[vk::binding(3, 0)]] RWByteAddressBuffer                        indirect_cmds;
[[vk::binding(4, 0)]] RWStructuredBuffer<InstanceData>           instance_data;
[[vk::binding(5, 0)]] RWByteAddressBuffer                        draw_counts;

// Hi-Z bindings (dummy 1x1 when hiz_enabled == 0)
[[vk::binding(6, 0)]] Texture2D<float>      hiz_pyramid;
[[vk::binding(7, 0)]] SamplerState          hiz_sampler;

// Active object IDs (contiguous, no gaps from free-list) with draw group assignment
[[vk::binding(8, 0)]] StructuredBuffer<ActiveIdEntry> active_ids;

// Draw group metadata (instance_base, max_instances per group)
[[vk::binding(9, 0)]] StructuredBuffer<DrawGroupGPU> draw_groups;

[numthreads(256, 1, 1)]
void compMain(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= cull_params.object_count)
		return;

	// Look up real GPU object ID from contiguous active IDs list
	uint idx = active_ids[tid.x].gpu_id;

	ObjectDataGPU obj = objects[idx];

	// Determine bucket from material flags
	uint alpha_mode = obj.material_flags & MAT_FLAG_ALPHA_MODE_MASK;
	bool is_mask = (alpha_mode == 1);
	bool is_double_sided = (obj.material_flags & MAT_FLAG_DOUBLE_SIDED) != 0;
	bool is_transparent = (obj.object_flags & OBJ_FLAG_IS_TRANSPARENT) != 0;

	// Shadow pass filtering
	if (cull_params.is_shadow_pass != 0) {
		if (is_transparent)
			return;
		if ((obj.object_flags & OBJ_FLAG_NO_SHADOW) != 0)
			return;
		bool is_dynamic_obj = (obj.object_flags & OBJ_FLAG_DYNAMIC) != 0;
		if (cull_params.is_shadow_pass == 2 && is_dynamic_obj)
			return;
		if (cull_params.is_shadow_pass == 3 && !is_dynamic_obj)
			return;
	}

	TransformGPU xform = transforms[idx];

	// Sphere precull
	float3 local_center = (obj.aabb_min.xyz + obj.aabb_max.xyz) * 0.5;
	float3 local_extent = (obj.aabb_max.xyz - obj.aabb_min.xyz) * 0.5;
	float3 center_world = mul(xform.world_transform, float4(local_center, 1.0)).xyz;
	float max_scale = extractMaxScale(xform.world_transform);
	float radius = length(local_extent) * max_scale;

	if (!frustumTest(cull_params, center_world, radius))
		return;

	float3 corners_world[8];
	transformAabbCorners(xform.world_transform, obj.aabb_min.xyz, obj.aabb_max.xyz, corners_world);
	float3 world_min, world_max;
	aabbBounds(corners_world, world_min, world_max);

	if (!aabbFrustumTest(cull_params, world_min, world_max))
		return;

	if (cull_params.hiz_enabled != 0 && cull_params.is_shadow_pass == 0)
		if (!hizOcclusionTestAabb(cull_params, hiz_pyramid, hiz_sampler, corners_world))
			return;

	uint lod = selectLod(cull_params, center_world, radius, obj.lod_count);

	// Stats tracking (main pass only). Reduce per-wave first, then one atomic per bucket
	// from the first lane to avoid contention
	if (cull_params.is_shadow_pass == 0) {
		uint bucket;
		if (is_transparent)
			bucket = 4u + (is_double_sided ? 1u : 0u);
		else
			bucket = (is_mask ? 2u : 0u) + (is_double_sided ? 1u : 0u);
		uint idx_count = obj.lod_index_count[lod];

		uint wave_idx_sum = WaveActiveSum(idx_count);
		uint wave_bucket[GPU_CULL_BUCKET_COUNT];
		[unroll]
		for (uint b = 0; b < GPU_CULL_BUCKET_COUNT; b++)
			wave_bucket[b] = WaveActiveCountBits(bucket == b);

		if (WaveIsFirstLane()) {
			[unroll]
			for (uint b = 0; b < GPU_CULL_BUCKET_COUNT; b++)
				if (wave_bucket[b] > 0)
					draw_counts.InterlockedAdd(b * 4, wave_bucket[b]);
			if (wave_idx_sum > 0)
				draw_counts.InterlockedAdd(GPU_CULL_BUCKET_COUNT * 4, wave_idx_sum);
		}
	}

	uint draw_group_id = obj.lod_draw_group_id[lod];
	DrawGroupGPU group = draw_groups[draw_group_id];

	// Wave-aggregated, bounded reservation of instanceCount.
	// VkDrawIndexedIndirectCommand layout (20 bytes):
	//   +0: indexCount, +4: instanceCount, +8: firstIndex, +12: vertexOffset, +16: firstInstance
	uint cmd_byte_offset = draw_group_id * 20 + 4;
	uint local_idx = 0;
	bool got_slot = false;
	bool pending = true;

	[allow_uav_condition]
	[loop]
	while (WaveActiveAnyTrue(pending)) {
		uint pick = WaveActiveMin(pending ? draw_group_id : 0xFFFFFFFFu);
		bool member = pending && (draw_group_id == pick);

		uint members = WaveActiveCountBits(member);
		uint my_pos  = WavePrefixCountBits(member);

		uint base = 0;
		if (member && my_pos == 0) {
			uint cur = indirect_cmds.Load(cmd_byte_offset);
			[allow_uav_condition]
			[loop]
			while (true) {
				uint want = min(cur + members, group.max_instances);
				if (want == cur) {
					base = cur;
					break;
				}
				uint orig;
				indirect_cmds.InterlockedCompareExchange(cmd_byte_offset, cur, want, orig);
				if (orig == cur) {
					base = cur;
					break;
				}
				cur = orig;
			}
		}
		base = WaveActiveMax(member ? base : 0);

		if (member) {
			uint granted = min(base + members, group.max_instances) - base;
			if (my_pos < granted) {
				local_idx = base + my_pos;
				got_slot = true;
			}
			pending = false;
		}
	}

	if (!got_slot)
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
