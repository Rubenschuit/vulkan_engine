
struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_color : COLOR;
	float2 in_tex_coord : TEXCOORD0;
};

struct UniformBuffer {
    float4x4 model;
    float4x4 view;
    float4x4 proj;
};
ConstantBuffer<UniformBuffer> ubo;

struct VertexOutput {
    float4 pos : SV_Position;
	float3 frag_color;
	float2 frag_tex_coord;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput output;
    output.pos = mul(ubo.proj, mul(ubo.view, mul(ubo.model, float4(input.in_pos, 1.0))));
    output.frag_color = input.in_color;
	output.frag_tex_coord = input.in_tex_coord;
    return output;
}

Sampler2D texture;

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
    return float4(in_vert.frag_color * texture.Sample(in_vert.frag_tex_coord).rgb, 1.0);
}
