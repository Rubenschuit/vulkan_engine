[vk::binding(0, 0)]
Sampler2D input_texture;

struct BloomUpsamplePushConstant {
    float filter_radius;
};

[vk::push_constant]
BloomUpsamplePushConstant push;

struct VertexOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertMain(uint vertexIndex : SV_VertexID) {
    VertexOutput output;
    float2 pos = float2((vertexIndex << 1) & 2, vertexIndex & 2);
    output.pos = float4(pos * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = pos;
    return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_Target {
    // The filter radius is in texel space.
    float x = push.filter_radius;
    float y = push.filter_radius;

    // Take 9 samples around current texel:
    // a - b - c
    // d - e - f
    // g - h - i
	// with corresponding weights:
	// 1 2 1
	// 2 4 2
	// 1 2 1
	// for a total weight of 16.

    float3 a = input_texture.Sample(input.uv + float2(-x, y)).rgb;
    float3 b = input_texture.Sample(input.uv + float2(0, y)).rgb;
    float3 c = input_texture.Sample(input.uv + float2(x, y)).rgb;

    float3 d = input_texture.Sample(input.uv + float2(-x, 0)).rgb;
    float3 e = input_texture.Sample(input.uv + float2(0, 0)).rgb;
    float3 f = input_texture.Sample(input.uv + float2(x, 0)).rgb;

    float3 g = input_texture.Sample(input.uv + float2(-x, -y)).rgb;
    float3 h = input_texture.Sample(input.uv + float2(0, -y)).rgb;
    float3 i = input_texture.Sample(input.uv + float2(x, -y)).rgb;

    float3 upsample = e * 4.0;
    upsample += (b + d + f + h) * 2.0;
    upsample += (a + c + g + i);
    upsample *= 0.0625; // divide by 16.0

    return float4(upsample, 1.0);
}
