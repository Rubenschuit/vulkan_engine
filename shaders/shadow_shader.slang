// Shadow mapping shader - depth-only pass from light's perspective
// Per-instance transforms read from SSBO, indexed by SV_VulkanInstanceID + instance_offset

struct VertexInput {
	float3 in_pos : POSITION;
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
	float4x4 projection_view;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> ubo;

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
	[vk::offset(0)] uint32_t instance_offset;  // SSBO offset
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
	float4 pos : SV_Position;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input, uint instance_id : SV_VulkanInstanceID) {
	VertexOutput output;

	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	// Meshlet path packs gpu_id (low 16) + meshlet_idx (high 16) into firstInstance;
	// GPU-cull path writes gpu_id directly. Masking is a no-op when gpu_id < 65536.
	uint gpu_id = (instance_id & 0xFFFFu) + push_constants.instance_offset;
	float4 world_pos = mul(instances[gpu_id].transform, pos);

	output.pos = mul(ubo.projection_view, world_pos); // pre-computed projection and view

	return output;
}

[shader("fragment")]
void fragMain(VertexOutput in_vert) {
	// Depth-only pass
}
