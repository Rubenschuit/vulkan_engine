// Depth-only prepass using the main render pass InstanceData (128 bytes).
// Shares Set 0 (UBO + instance SSBO + material SSBO) with PBR.

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

struct VertexOutput {
	float4 pos : SV_Position;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input, uint instance_id : SV_VulkanInstanceID) {
	VertexOutput output;

	float4 pos = float4(input.in_pos, 1.0);
	float4 world_pos = mul(instances[instance_id].transform, pos);
	output.pos = mul(ubo.projection_view, world_pos);

	return output;
}

[shader("fragment")]
void fragMain(VertexOutput in_vert) {
	// Depth-only pass
}
