// Editor debug overlay 

struct VertexInput {
    float3 in_pos : POSITION;
    float4 in_color : COLOR0;
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> ubo;

struct VertexOutput {
    float4 pos : SV_Position;
    float4 color;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput outV;
    outV.pos = mul(ubo.proj, mul(ubo.view, float4(input.in_pos, 1.0)));
    outV.color = input.in_color;
    return outV;
}

[shader("fragment")]
float4 fragMain(VertexOutput inV) : SV_Target {
    return inV.color;
}