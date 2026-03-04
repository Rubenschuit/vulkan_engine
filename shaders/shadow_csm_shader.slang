// CSM multiview shadow shader — renders all cascades in a single pass via SV_ViewID
// Per-instance transforms read from SSBO, indexed by SV_VulkanInstanceID + instance_offset

#include "ve_constants.slangh"

struct VertexInput {
	float3 in_pos : POSITION;
};

struct CsmUniformBuffer {
	float4x4 view[NUM_CSM_CASCADES];
	float4x4 proj[NUM_CSM_CASCADES];
	float4x4 projection_view[NUM_CSM_CASCADES];
};
[vk::binding(0, 0)]
ConstantBuffer<CsmUniformBuffer> ubo;

// Per-instance transforms stored in SSBO (must match 128-byte C++ InstanceData)
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
[vk::binding(1, 0)]
StructuredBuffer<InstanceData> instances;

struct PushConstantData {
	[vk::offset(0)] uint32_t instance_offset;
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
	float4 pos : SV_Position;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input, uint instance_id : SV_VulkanInstanceID, uint view_id : SV_ViewID) {
	VertexOutput output;

	float4 pos = float4(input.in_pos, 1.0);
	uint gpu_id = (instance_id & 0xFFFFu) + push_constants.instance_offset;
	float4 world_pos = mul(instances[gpu_id].transform, pos);

	output.pos = mul(ubo.projection_view[view_id], world_pos);

	return output;
}

[shader("fragment")]
void fragMain(VertexOutput in_vert) {
	// Depth-only pass
}
