// Render a simple camera-facing quad for each point light in the scene
static const float2 vertices[6] = float2[](
	float2( 0.5, -0.5),
	float2(-0.5, -0.5),
	float2( 0.5,  0.5),
	float2( 0.5,  0.5),
	float2(-0.5, -0.5),
	float2(-0.5,  0.5)
);

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	float4 position;
	float4 color; // .w = intensity
	float scale; // in camera space units
};
[push_constant]
PushConstantData push;

struct VertexOutput {
    float4 pos: SV_Position;
	float2 offset;
};

[shader("vertex")]
VertexOutput vertMain(uint id : SV_VertexID) {
    VertexOutput out;

	out.offset = vertices[id] + 0.5f;

	float4 position_cameraspace = mul(ubo.view, push.position);
	position_cameraspace += float4(vertices[id] * push.scale, 0.0f, 0.0f);

 	out.pos = mul(ubo.proj, position_cameraspace);

    return out;
}

[vk::binding(0, 1)]
Sampler2D glow_texture;
[vk::binding(1, 1)]
Sampler2D fire_texture;
[vk::binding(2, 1)]
Sampler2D smoke_texture;

[shader("fragment")]
float4 fragMain(VertexOutput in) : SV_Target {

	float4 col = glow_texture.Sample(in.offset);
	float intensity = clamp(push.color.a, 0.0f, 200.0f);
	return col * float4(push.color.rgb * intensity, 1.0f);
}
