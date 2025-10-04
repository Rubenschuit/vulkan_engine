
struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_color : COLOR;
	float2 in_tex_coord : TEXCOORD0;
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
	float3 offset;
};
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	float4x4 transform;
	float3 color;
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
    float4 pos : SV_Position;
	float3 frag_color;
	float2 frag_tex_coord;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput output;
	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	pos = mul(push_constants.transform, pos); // apply per-object transform
	pos += float4(ubo.offset, 0.0f); // global translation
    output.pos = mul(ubo.proj, mul(ubo.view, pos));
    output.frag_color = input.in_color;
	output.frag_tex_coord = input.in_tex_coord;
    return output;
}

Sampler2D texture;

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
	float4 c = texture.Sample(in_vert.frag_tex_coord);
	// Discard fully transparent texels so they don't write depth or color
	if (c.a <= 0.001)
		discard;
	c.a = 0.9f;
	return c;
}
