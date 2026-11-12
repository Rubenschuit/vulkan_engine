// Pass-2 meshlet culling: per-meshlet frustum + backface cone + Hi-Z.
// Each thread maps to one meshlet via direct lookup in meshlet_object_map
// (written by pass 1). Surviving meshlets emit one VkDrawIndexedIndirectCommand
// into the appropriate bucket region of the indirect buffer.
//
// Dispatch: vkCmdDispatchIndirect (groups computed by pass 1 via InterlockedMax)

#include "ve_constants.slangh"
#include "cull_common.slangh"

// 1 in the Meshlet ID view's pipeline: firstInstance also carries the meshlet's colour
[[vk::constant_id(5)]] const uint VE_MESHLET_COLOR = 0;

[[vk::binding(0,  0)]] StructuredBuffer<VisibleObjectEntry> visible_objects;
[[vk::binding(1,  0)]] StructuredBuffer<uint2>              meshlet_object_map; // (vis_idx, local_idx)
[[vk::binding(2,  0)]] RWByteAddressBuffer                  counts;        // [0]=visible_count, [1]=total_meshlets
[[vk::binding(3,  0)]] StructuredBuffer<MeshletGPU>         meshlet_ssbo;
[[vk::binding(4,  0)]] StructuredBuffer<ObjectDataGPU>      objects;
[[vk::binding(5,  0)]] StructuredBuffer<TransformGPU>       transforms;
[[vk::binding(6,  0)]] ConstantBuffer<CullParams>           cull_params;

// Hi-Z bindings (dummy 1x1 when hiz_enabled == 0)
[[vk::binding(7,  0)]] Texture2D<float>  hiz_pyramid;
[[vk::binding(8,  0)]] SamplerState      hiz_sampler;

[[vk::binding(9,  0)]] RWByteAddressBuffer indirect_cmds;  // VkDrawIndexedIndirectCommand[]
[[vk::binding(10, 0)]] RWByteAddressBuffer draw_counts;    // per-bucket surviving meshlet counts

[[vk::binding(11, 0)]] StructuredBuffer<uint> skinned_dynamic_offsets;

[numthreads(MESHLET_CULL_WORKGROUP_SIZE, 1, 1)]
void compMain(uint3 tid : SV_DispatchThreadID) {
	uint total_meshlets = min(counts.Load(4), cull_params.max_meshlet_draws);
	if (tid.x >= total_meshlets)
		return;

	// Direct lookup from indirection map written by pass 1
	uint2 entry    = meshlet_object_map[tid.x];
	uint obj_idx   = entry.x;
	uint local_idx = entry.y;

	VisibleObjectEntry vis = visible_objects[obj_idx];

	MeshletGPU m = meshlet_ssbo[vis.meshlet_offset + local_idx];
	TransformGPU xform = transforms[vis.gpu_id];

	// Transform meshlet bounding sphere to world space
	float3 center_world = mul(xform.world_transform, float4(m.bounding_sphere.xyz, 1.0)).xyz;
	float max_scale = extractMaxScale(xform.world_transform);
	float radius = m.bounding_sphere.w * max_scale;

	// Frustum cull
	if (!frustumTest(cull_params, center_world, radius))
		return;

	ObjectDataGPU obj = objects[vis.gpu_id];
	bool is_double_sided = (obj.material_flags & MAT_FLAG_DOUBLE_SIDED) != 0;

	// Backface cone cull (skip for shadow passes and double-sided objects)
	if (cull_params.is_shadow_pass == 0 && !is_double_sided && m.cone_axis_cutoff.w < 1.0) {
		float3 apex_world = mul(xform.world_transform, float4(m.cone_apex.xyz, 1.0)).xyz;
		float3 axis_world = normalize(mul(xform.world_transform, float4(m.cone_axis_cutoff.xyz, 0.0)).xyz);

		float3 cam_pos = cull_params.camera_pos.xyz;
		float3 view_dir = normalize(apex_world - cam_pos);
		if (dot(view_dir, axis_world) >= m.cone_axis_cutoff.w)
			return;
	}

	// Hi-Z occlusion cull
	if (cull_params.hiz_enabled != 0 && cull_params.is_shadow_pass == 0)
		if (!hizOcclusionTest(cull_params, hiz_pyramid, hiz_sampler, center_world, radius))
			return;

	// Determine bucket from object material flags
	bool is_mask         = ((obj.material_flags & MAT_FLAG_ALPHA_MODE_MASK) == 1);
	bool is_transparent  = (obj.object_flags & OBJ_FLAG_IS_TRANSPARENT) != 0;
	uint bucket;
	if (is_transparent)
		bucket = 4u + (is_double_sided ? 1u : 0u);
	else
		bucket = (is_mask ? 2u : 0u) + (is_double_sided ? 1u : 0u);

	// Claim a slot within this bucket
	uint slot;
	draw_counts.InterlockedAdd(bucket * 4, 1, slot);
	if (cull_params.is_shadow_pass == 0) {
		// Triangle counter (performance panel)
		uint wave_tris = WaveActiveSum(m.index_count);
		if (WaveIsFirstLane())
			draw_counts.InterlockedAdd(cull_params.bucket_count * 4, wave_tris);
	}

	uint max_per_bucket = cull_params.max_meshlet_draws / cull_params.bucket_count;
	if (slot >= max_per_bucket)
		return;

	uint abs_slot = bucket * max_per_bucket + slot;

	uint dyn = skinned_dynamic_offsets[vis.gpu_id];
	uint vertex_offset_to_write = (dyn != 0u) ? dyn : m.vertex_offset;

	// Write VkDrawIndexedIndirectCommand (20 bytes):
	//   +0: indexCount, +4: instanceCount, +8: firstIndex, +12: vertexOffset, +16: firstInstance
	uint byte_off = abs_slot * 20;
	indirect_cmds.Store(byte_off + 0,  m.index_count);
	indirect_cmds.Store(byte_off + 4,  1u);
	indirect_cmds.Store(byte_off + 8,  m.index_offset);
	indirect_cmds.Store(byte_off + 12, vertex_offset_to_write);
	// Instance data lives at gpu_id (pass 1), and instanceCount is 1, so the vertex shader's
	// SV_VulkanInstanceID is gpu_id.
	uint first_instance = vis.gpu_id;
	if (VE_MESHLET_COLOR != 0)
		first_instance |= m.debug_color << MESHLET_COLOR_SHIFT;
	indirect_cmds.Store(byte_off + 16, first_instance);
}