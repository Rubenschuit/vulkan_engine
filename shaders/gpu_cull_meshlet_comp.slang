// Pass-1 meshlet culling: per-object frustum + Hi-Z + LOD selection.
// Surviving objects write a VisibleObjectEntry, per-object InstanceData,
// and fill the meshlet-to-object indirection map for pass 2.
// Also computes the pass-2 dispatch size via InterlockedMax on dispatch_indirect.
//
// Dispatch: (ceil(object_count / MESHLET_CULL_WORKGROUP_SIZE), 1, 1)

#include "ve_constants.slangh"
#include "cull_common.slangh"

[[vk::binding(0,  0)]] StructuredBuffer<ObjectDataGPU>        objects;
[[vk::binding(1,  0)]] StructuredBuffer<TransformGPU>         transforms;
[[vk::binding(2,  0)]] ConstantBuffer<CullParams>             cull_params;
[[vk::binding(3,  0)]] StructuredBuffer<ActiveIdEntry>        active_ids;
[[vk::binding(4,  0)]] StructuredBuffer<MeshletObjectInfo>    meshlet_obj_info;
[[vk::binding(5,  0)]] RWStructuredBuffer<VisibleObjectEntry> visible_objects;
[[vk::binding(6,  0)]] RWByteAddressBuffer                    counts;      // [0]=visible_count, [1]=total_meshlets
[[vk::binding(7,  0)]] RWStructuredBuffer<InstanceData>       instance_data;

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
	TransformGPU xform = transforms[gpu_id];

	// Transform bounding sphere to world space
	float3 center_world = mul(xform.world_transform, float4(obj.bounding_sphere.xyz, 1.0)).xyz;
	float max_scale = extractMaxScale(xform.world_transform);
	float radius = obj.bounding_sphere.w * max_scale;

	// Frustum cull
	if (!frustumTest(cull_params, center_world, radius))
		return;

	// Hi-Z occlusion cull (prev-frame)
	if (cull_params.hiz_enabled != 0)
		if (!hizOcclusionTest(cull_params, hiz_pyramid, hiz_sampler, center_world, radius))
			return;

	// Skip transparent objects in shadow passes
	bool is_transparent = (obj.material_flags & 0x20) != 0;
	if (is_transparent && cull_params.is_shadow_pass != 0)
		return;

	// Skip objects marked no_shadow
	if ((obj.material_flags & 0x40) != 0 && cull_params.is_shadow_pass != 0)
		return;

	// LOD selection
	uint lod = selectLod(cull_params, center_world, radius, obj.lod_count);

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

	// Write per-object InstanceData at gpu_id index.
	// Pass-2 draw commands set firstInstance = gpu_id so the vertex shader reads
	// instance_data[SV_VulkanInstanceIndex] = instance_data[gpu_id].
	bool is_mask = ((obj.material_flags & 0x3) == 1);

	InstanceData inst;
	inst.transform      = xform.world_transform;
	inst.normal_col0    = xform.normal_col0;
	inst.normal_col1    = xform.normal_col1;
	inst.normal_col2    = xform.normal_col2;
	inst.material_index = obj.material_index;
	inst.lod_level      = lod;
	inst.depth_offset   = is_mask ? 0.00001 : 0.0;
	inst.material_flags = obj.material_flags;
	instance_data[gpu_id] = inst;
}