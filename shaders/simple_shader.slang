
struct VertexInput {
	float2 in_pos : POSITION;
	float3 in_color : COLOR;
};

struct VertexOutput {
    float3 color;
	float2 pos;
    float4 sv_position : SV_Position;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput output;
    float2 p = input.in_pos;
    output.sv_position = float4(p, 0.0, 1.0);
    output.pos = p;
    output.color = input.in_color;
    return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput inVert) : SV_Target
{
    return float4(inVert.color, 1.0);
}
