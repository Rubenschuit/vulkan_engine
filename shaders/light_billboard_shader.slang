// Render camera-facing billboard quads for light sources:
//   type 0: point light
//   type 1: moon (bluish tint, moderate intensity)
//   type 2: sun  (warm tint, moderate intensity)
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
	float4 color;          // .w = intensity
	float scale;           // in camera space units
	uint billboard_type;   // 0 = point light, 1 = moon, 2 = sun
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

[shader("fragment")]
float4 fragMain(VertexOutput in) : SV_Target {
	float4 col = glow_texture.Sample(in.offset);
	float intensity = clamp(push.color.a, 0.0f, 200.0f);

	float3 color = push.color.rgb;

	// Apply tint for celestial bodies
	if (push.billboard_type == 1) {
		// Moon: bluish tint
		color *= float3(0.5f, 0.55f, 1.0f);
	} else if (push.billboard_type == 2) {
		// Sun: warm tint
		color *= float3(1.0f, 0.9f, 0.7f);
	}

	return col * float4(color * intensity, 1.0f);
}
