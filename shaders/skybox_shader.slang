struct VertexInput {
	float3 in_pos : POSITION;
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
};
[vk::binding(0, 0)] // binding 0, set 0
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	float4x4 transform;
	float4 params;  // x=exposure, yzw=tint (RGB)
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
	float4 pos : SV_Position;
	float3 cube_dir;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
	VertexOutput output;

	// Convert Y-up to engine Z-up
	output.cube_dir = normalize(float3(input.in_pos.x, input.in_pos.z, input.in_pos.y));
	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	float4 world_pos = mul(push_constants.transform, pos);
	output.pos = mul(ubo.proj, mul(ubo.view, world_pos)); // view and projection

	return output;
}

[vk::binding(0, 1)] // binding 0, set 1
SamplerCube cube_texture;

[shader("fragment")]
float4 fragMain(VertexOutput in_vert, uint id : SV_SampleIndex) : SV_Target {
	float4 sampled = cube_texture.Sample(in_vert.cube_dir);
	float exposure = push_constants.params.x;
	float3 tint = push_constants.params.yzw;
	return float4(sampled.rgb * exposure * tint, sampled.a);
}
