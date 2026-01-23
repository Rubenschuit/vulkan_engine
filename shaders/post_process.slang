[vk::binding(0, 0)]
Sampler2D input_texture;

struct PostProcessPushConstant {
    int blur_radius;
    float blur_strength;
};

[vk::push_constant]
PostProcessPushConstant push;

struct VertexOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertMain(uint vertexIndex : SV_VertexID) {
    VertexOutput output;

    // Fullscreen triangle to cover the screen [-1, 1] X [-1, 1]:
	// Anything outside the viewport is discarded and will not reach the fragment shader.
    // Index 0: (-1, -1) and UV (0, 0)
    // Index 1: ( 3, -1) and UV (2, 0)
    // Index 2: (-1,  3) and UV (0, 2)

	// convert vertex index to UV coordinates
    float2 pos = float2((vertexIndex << 1) & 2, vertexIndex & 2);
	// convert UV coordinates to NDC coordinates for the triangle
    output.pos = float4(pos * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = pos;

    return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_Target {
	// no blur
    if (push.blur_radius <= 0) {
        return float4(input_texture.Sample(input.uv).rgb, 1.0);
    }

	// blur
    uint width, height;
    input_texture.GetDimensions(width, height);
    float2 texel_size = 1.0 / float2(width, height);

    float3 result = 0.0;
    float total_weight = 0.0;

    for (int i = -push.blur_radius; i <= push.blur_radius; i++) {
        for (int j = -push.blur_radius; j <= push.blur_radius; j++) {
            float2 offset = float2(float(i), float(j)) * texel_size * push.blur_strength;

            float sigma = float(push.blur_radius);
            float weight = exp(-(float(i*i + j*j)) / (2.0 * sigma * sigma));

            result += input_texture.Sample(input.uv + offset).rgb * weight;
            total_weight += weight;
        }
    }

	// total_weight should not be 0 since blur_radius is not 0
    return float4(result / total_weight, 1.0);
}

