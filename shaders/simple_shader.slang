
struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_color : COLOR;
	float2 in_tex_coord : TEXCOORD0;
};

struct UniformBuffer {
    float4x4 model;
    float4x4 view;
    float4x4 proj;
	float3 offset;
};
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	float3 offset;
	float3 scale;
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
	float4 pos = mul(ubo.model, float4(input.in_pos, 1.0));
	pos *= float4(push_constants.scale, 1.0f); // scale per object
	pos += float4(push_constants.offset, 0.0f); // Translate per object
	pos += float4(ubo.offset, 0.0f); // global translation

    output.pos = mul(ubo.proj, mul(ubo.view, pos));


    output.frag_color = input.in_color;
	output.frag_tex_coord = input.in_tex_coord;
    return output;
}

Sampler2D texture;

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
    return float4(texture.Sample(in_vert.frag_tex_coord).rgb, 1.0);
}
