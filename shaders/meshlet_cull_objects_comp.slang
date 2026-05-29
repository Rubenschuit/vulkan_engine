// Pass-1 meshlet culling: per-object frustum + Hi-Z + LOD selection.
// Surviving objects write a VisibleObjectEntry, per-object InstanceData,
// and fill the meshlet-to-object indirection map for pass 2.
// Also computes the pass-2 dispatch size via InterlockedMax on dispatch_indirect.
//
// Dispatch: (ceil(object_count / MESHLET_CULL_WORKGROUP_SIZE), 1, 1)

#include "ve_constants.slangh"
#include "cull_common.slangh"

[[vk::constant_id(4)]] const uint VE_CULL_IS_SHADOW = 0;

[[vk::binding(0,  0)]] StructuredBuffer<ObjectDataGPU>        objects;
[[vk::binding(1,  0)]] StructuredBuffer<TransformGPU>         transforms;
[[vk::binding(2,  0)]] ConstantBuffer<CullParams>             cull_params;
[[vk::binding(3,  0)]] StructuredBuffer<ActiveIdEntry>        active_ids;
[[vk::binding(4,  0)]] StructuredBuffer<MeshletObjectInfo>    meshlet_obj_info;
[[vk::binding(5,  0)]] RWStructuredBuffer<VisibleObjectEntry> visible_objects;
[[vk::binding(6,  0)]] RWByteAddressBuffer                    counts;      // [0]=visible_count, [1]=total_meshlets
[[vk::binding(7,  0)]] RWByteAddressBuffer                    instance_data;

// Hi-Z bindings (dummy 1x1 when hi z disabled)
[[vk::binding(8,  0)]] Texture2D<float>  hiz_pyramid;
[[vk::binding(9,  0)]] SamplerState      hiz_sampler;

// Meshlet indirection map for pass 2: packed (vis_idx << 16 | local_meshlet_idx)
[[vk::binding(10, 0)]] RWStructuredBuffer<uint>  meshlet_object_map;
// Pass-2 dispatch indirect command (groups_x written via InterlockedMax)
[[vk::binding(11, 0)]] RWByteAddressBuffer       dispatch_indirect;

[numthreads(MESHLET_CULL_WORKGROUP_SIZE, 1, 1)]
void compMain(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= cull_params.object_count)
		return;

	// Look up real GPU object ID from contiguous active IDs list
	uint gpu_id = active_ids[tid.x].gpu_id;

	ObjectDataGPU obj = objects[gpu_id];

	if (VE_CULL_IS_SHADOW != 0) {
		if ((obj.object_flags & OBJ_FLAG_IS_TRANSPARENT) != 0)
			return;
		if ((obj.object_flags & OBJ_FLAG_NO_SHADOW) != 0)
			return;
		bool is_dynamic_obj = (obj.object_flags & OBJ_FLAG_DYNAMIC) != 0;
		if (cull_params.is_shadow_pass == 2 && is_dynamic_obj)
			return;
		if (cull_params.is_shadow_pass == 3 && !is_dynamic_obj)
			return;
	}

	TransformGPU xform = transforms[gpu_id];

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

	if (VE_CULL_IS_SHADOW == 0 && cull_params.hiz_enabled != 0)
		if (!hizOcclusionTestAabb(cull_params, hiz_pyramid, hiz_sampler, corners_world))
			return;

	uint lod = selectLod(cull_params, center_world, radius, obj.lod_count, VE_CULL_IS_SHADOW != 0);

	// Look up meshlet range for this object + LOD
	MeshletObjectInfo moi = meshlet_obj_info[gpu_id];
	uint meshlet_count = moi.meshlet_count[lod];
	if (meshlet_count == 0)
		return;

	// Atomically claim a slot in the visible_objects array
	uint vis_idx;
	counts.InterlockedAdd(0, 1, vis_idx);
	if (vis_idx >= MAX_GPU_OBJECTS)
		return;

	// Atomically claim a range in the meshlet indirection map
	uint prefix;
	counts.InterlockedAdd(4, meshlet_count, prefix);

	// Clamp writes to max_meshlet_draws
	uint max_meshlets = cull_params.max_meshlet_draws;
	uint end = min(prefix + meshlet_count, max_meshlets);
	uint actual_count = (prefix < max_meshlets) ? (end - prefix) : 0;

	// Write visible object entry for pass 2
	VisibleObjectEntry entry;
	entry.gpu_id         = gpu_id;
	entry.lod_level      = lod;
	entry.meshlet_offset = moi.meshlet_offset[lod];
	entry.meshlet_count  = meshlet_count;
	visible_objects[vis_idx] = entry;

	// Fill meshlet-to-object indirection: pack (vis_idx, local_meshlet_idx)
	for (uint m = 0; m < actual_count; m++)
		meshlet_object_map[prefix + m] = (vis_idx << 16) | m;

	// Update pass-2 dispatch group count via InterlockedMax
	if (actual_count > 0) {
		uint new_total = prefix + actual_count;
		uint groups_x = (new_total + MESHLET_CULL_WORKGROUP_SIZE - 1) / MESHLET_CULL_WORKGROUP_SIZE;
		dispatch_indirect.InterlockedMax(0, groups_x);
	}

	// Write per-object instance data at gpu_id index. Pass-2 draw commands set
	// firstInstance = gpu_id so the vertex shader reads
	// instance_data[SV_VulkanInstanceIndex] = instance_data[gpu_id].
	float4 c0 = xform.world_transform._m00_m10_m20_m30;
	float4 c1 = xform.world_transform._m01_m11_m21_m31;
	float4 c2 = xform.world_transform._m02_m12_m22_m32;
	float4 c3 = xform.world_transform._m03_m13_m23_m33;

	if (VE_CULL_IS_SHADOW != 0) {
		uint base = gpu_id * 64u;
		instance_data.Store4(base +  0u, asuint(c0));
		instance_data.Store4(base + 16u, asuint(c1));
		instance_data.Store4(base + 32u, asuint(c2));
		instance_data.Store4(base + 48u, asuint(c3));
	} else {
		bool is_mask = ((obj.material_flags & MAT_FLAG_ALPHA_MODE_MASK) == 1);
		uint base = gpu_id * 128u;
		instance_data.Store4(base +   0u, asuint(c0));
		instance_data.Store4(base +  16u, asuint(c1));
		instance_data.Store4(base +  32u, asuint(c2));
		instance_data.Store4(base +  48u, asuint(c3));
		instance_data.Store4(base +  64u, asuint(xform.normal_col0));
		instance_data.Store4(base +  80u, asuint(xform.normal_col1));
		instance_data.Store4(base +  96u, asuint(xform.normal_col2));
		uint depth_offset_bits = asuint(is_mask ? 0.00001f : 0.0f);
		uint4 packed = uint4(obj.material_index, lod, depth_offset_bits, obj.material_flags);
		instance_data.Store4(base + 112u, packed);
	}
}