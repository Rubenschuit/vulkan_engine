// Outline mask shader: renders target meshes as white silhouette (R8_UNORM)

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
VertexOutput vertMain(VertexInput input, uint instance_id : SV_InstanceID) {
	VertexOutput output;
	float4 pos = float4(input.in_pos, 1.0);
	float4 world_pos = mul(instances[instance_id + push_constants.instance_offset].transform, pos);
	output.pos = mul(ubo.projection_view, world_pos);
	return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
	return float4(1.0, 0.0, 0.0, 0.0);
}