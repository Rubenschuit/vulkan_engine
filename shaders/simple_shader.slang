// Consolidated Slang shader with two entry points used by the engine
// VSMain -> vertex stage, PSMain -> fragment stage

struct VSOut {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
};

VSOut vertMain(uint VertexIndex : SV_VertexID)
{
    float2 positions[3] = {
        float2(0.0, -0.5),
        float2(0.5, 0.5),
        float2(-0.5, 0.5)
    };
    float3 colors[3] = {
        float3(1,0,0),
        float3(0,1,0),
        float3(0,0,1)
    };

    VSOut o;
    o.position = float4(positions[VertexIndex], 0.0, 1.0);
    o.color = colors[VertexIndex];
    return o;
}

float4 fragMain(VSOut i) : SV_Target0
{
    // If you later feed varying color, this will render a gradient triangle.
    return float4(i.color, 1.0);
}
