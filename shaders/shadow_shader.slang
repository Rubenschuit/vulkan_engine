// Shadow mapping shader - depth-only pass from light's perspective
// Uses same vertex shader as simple_shader, fragment shader just writes depth

struct VertexInput {
	float3 in_pos : POSITION;
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	[vk::offset(0)]   float4x4 transform;      // 64
	[vk::offset(64)]  float4   normal_col0;    // 16
	[vk::offset(80)]  float4   normal_col1;    // 16
	[vk::offset(96)]  float4   normal_col2;    // 16
	[vk::offset(112)] float    has_texture;    // 4
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
	float4 pos : SV_Position;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
	VertexOutput output;

	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	float4 world_pos = mul(push_constants.transform, pos); // apply per-object transform

	output.pos = mul(ubo.proj, mul(ubo.view, world_pos)); // view and projection from light's perspective

	return output;
}

[shader("fragment")]
void fragMain(VertexOutput in_vert) {
	// Depth-only pass
}

