struct VertexInput {
    float3 in_pos : POSITION;
    float3 in_color : COLOR;
    float2 in_tex_coord : TEXCOORD0; // ignored
};

struct UniformBuffer {
    float4x4 model;
    float4x4 view;
    float4x4 proj;
    float3 offset; // global translation, optional
};
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
    float3 offset; // per-object translation
    float3 scale;  // per-object scale
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
    float4 pos : SV_Position;
    float3 color;
	float2 tex_coord; // unused
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput outV;
    outV.tex_coord = input.in_tex_coord; // unused
    float4 pos = float4(input.in_pos, 1.0);
    outV.pos = mul(ubo.proj, mul(ubo.view, pos)); // only view and projection
    outV.color = input.in_color;
    return outV;
}

[shader("fragment")]
float4 fragMain(VertexOutput inV) : SV_Target {
    return float4(inV.color, 1.0);
}
