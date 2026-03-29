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

	// Transform bounding sphere center to world space
	float3 center_local = obj.bounding_sphere.xyz;
	float3 center_world = mul(xform.world_transform, float4(center_local, 1.0)).xyz;
	float max_scale = extractMaxScale(xform.world_transform);
	float radius = obj.bounding_sphere.w * max_scale;

	if (!frustumTest(cull_params, center_world, radius))
		return;

	if (cull_params.hiz_enabled != 0 && cull_params.is_shadow_pass == 0)
		if (!hizOcclusionTest(cull_params, hiz_pyramid, hiz_sampler, center_world, radius))
			return;

	// Select LOD and resolve the corresponding draw group
	uint lod = selectLod(cull_params, center_world, radius, obj.lod_count);

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
