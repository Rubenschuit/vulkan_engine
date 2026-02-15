struct VertexInput {
    float3 in_pos : POSITION;
	float3 in_normal : NORMAL; // repurposed as per-vertex color
    float2 in_tex_coord : TEXCOORD0; // ignored
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> ubo;

struct VertexOutput {
    float4 pos : SV_Position;
    float3 color;
	float2 tex_coord; // unused
	float3 normal; // unused
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput outV;
    outV.tex_coord = input.in_tex_coord; // unused
	outV.normal = float3(0.0); // unused
    float4 pos = float4(input.in_pos, 1.0);
    outV.pos = mul(ubo.proj, mul(ubo.view, pos)); // only view and projection
    outV.color = input.in_normal; // normal field stores per-vertex color
    return outV;
}

[shader("fragment")]
float4 fragMain(VertexOutput inV) : SV_Target {
    return float4(inV.color, 1.0f);
}
