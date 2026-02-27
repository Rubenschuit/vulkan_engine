// GPU frustum culling compute shader.
// Each thread tests one object against 6 frustum planes.
// Survivors atomically append to per-bucket indirect command + instance data buffers.
//
// Dispatch: (ceil(object_count / 256), 1, 1)

#include "ve_constants.slangh"

struct ObjectDataGPU {
	float4   bounding_sphere; // local-space center, w = radius
	uint     vertex_offset;
	uint     material_index;
	uint     material_flags;  // bits 0-1: alpha_mode, bit 2: double_sided, bit 5: transparent
	uint     lod_count;
	uint     lod_first_index[MAX_LOD_LEVELS];
	uint     lod_index_count[MAX_LOD_LEVELS];
};

struct TransformGPU {
	float4x4 world_transform;
	float4   normal_col0;
	float4   normal_col1;
	float4   normal_col2;
};

struct DrawIndexedIndirectCommand {
	uint indexCount;
	uint instanceCount;
	uint firstIndex;
	int  vertexOffset;
	uint firstInstance;
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

struct CullParams {
	float4   frustum_planes[6]; // xyz = normal, w = distance
	float4x4 view_proj;
	uint     object_count;
	uint     bucket_stride;     // MAX_GPU_OBJECTS per bucket region
	uint     _pad0;
	uint     _pad1;
};

[[vk::binding(0, 0)]] StructuredBuffer<ObjectDataGPU>            objects;
[[vk::binding(1, 0)]] StructuredBuffer<TransformGPU>             transforms;
[[vk::binding(2, 0)]] ConstantBuffer<CullParams>                 cull_params;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> indirect_cmds;
[[vk::binding(4, 0)]] RWStructuredBuffer<InstanceData>           instance_data;
[[vk::binding(5, 0)]] RWByteAddressBuffer                        draw_counts;

float maxComponent(float3 v) {
	return max(max(abs(v.x), abs(v.y)), abs(v.z));
}

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

[numthreads(256, 1, 1)]
void compMain(uint3 tid : SV_DispatchThreadID) {
	uint idx = tid.x;
	if (idx >= cull_params.object_count)
		return;

	ObjectDataGPU obj = objects[idx];

	// Skip empty slots (zeroed bounding sphere from unregistered objects)
	if (obj.bounding_sphere.w <= 0.0)
		return;

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

	// Skip transparents (handled by CPU back-to-front sorting)
	if (is_transparent)
		return;

	uint bucket = (is_mask ? 2u : 0u) + (is_double_sided ? 1u : 0u);

	// LOD 0 for now (future: screen-space LOD selection)
	uint lod = 0;

	// Atomic append to bucket
	uint local_idx;
	draw_counts.InterlockedAdd(bucket * 4, 1, local_idx);
	uint cmd_idx = bucket * cull_params.bucket_stride + local_idx;

	// Write indirect draw command
	DrawIndexedIndirectCommand draw_cmd;
	draw_cmd.indexCount = obj.lod_index_count[lod];
	draw_cmd.instanceCount = 1;
	draw_cmd.firstIndex = obj.lod_first_index[lod];
	draw_cmd.vertexOffset = int(obj.vertex_offset);
	draw_cmd.firstInstance = cmd_idx;
	indirect_cmds[cmd_idx] = draw_cmd;

	// Write instance data
	InstanceData inst;
	inst.transform = xform.world_transform;
	inst.normal_col0 = xform.normal_col0;
	inst.normal_col1 = xform.normal_col1;
	inst.normal_col2 = xform.normal_col2;
	inst.material_index = obj.material_index;
	inst.lod_level = lod;
	inst.depth_offset = is_mask ? 0.00001 : 0.0;
	inst.material_flags = obj.material_flags;
	instance_data[cmd_idx] = inst;
}