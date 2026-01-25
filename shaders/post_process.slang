[vk::binding(0, 0)]
Sampler2D input_texture;
[vk::binding(1, 0)]
Sampler2D bloom_texture;

struct PostProcessPushConstant {
    int blur_radius;
    float blur_strength;
    float exposure;
    int color_space; // 0: SRGB, 1: Extended Linear, 2: HDR10 ST2084
    float bloom_strength;
	float padding[3];
    float2 texel_size;
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

    // Fullscreen triangle to cover the screen coordinates in range [-1, 1] X [-1, 1]:
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

float3 linearToPQ(float3 color) {
    color = max(0.0, color);
    float m1 = 2610.0 / 4096.0 / 4.0;
    float m2 = 2523.0 / 4096.0 * 128.0;
    float c1 = 3424.0 / 4096.0;
    float c2 = 2413.0 / 4096.0 * 32.0;
    float c3 = 2392.0 / 4096.0 * 32.0;
    float3 cp = pow(color, m1);
    return pow((c1 + c2 * cp) / (1.0 + c3 * cp), m2);
}

// Convert BT.709 (sRGB) to BT.2020
float3 bt709to2020(float3 color) {
    float3x3 m = {
        0.6274040, 0.3292854, 0.0433106,
        0.0690974, 0.9195410, 0.0113616,
        0.0163914, 0.0880132, 0.8955954
    };
    return mul(m, color);
}

[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_Target {
    float3 color;

	// no blur
    if (push.blur_radius <= 0) {
        color = input_texture.Sample(input.uv).rgb;
    } else {
	    // blur
        float2 texel_size = push.texel_size;

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
        color = result / total_weight;
    }

    // Exposure
    color *= push.exposure;

    // Bloom
	if (push.bloom_strength > 0.0) {
    	float3 bloom = bloom_texture.Sample(input.uv).rgb;
        //color = lerp(color, bloom, push.bloom_strength);
        color += bloom * push.bloom_strength;
    }

    if (push.color_space == 0) {
        // SDR (SRGB Nonlinear)
        // Keep it in [0, 1] range
        color = color / (color + float3(1.0));
        // Hardware handles gamma correction for _SRGB swapchain formats
    } else if (push.color_space == 1) {
        // HDR (Extended SRGB Linear)
        // We can allow values > 1.0.
    } else if (push.color_space == 2) {
        // HDR (HDR10 ST2084 / PQ)
        // Convert from sRGB/BT.709 primaries to BT.2020 primaries
        color = bt709to2020(color);

        // Map scene 1.0 brightness to a reasonable value in nits.
        color = linearToPQ(color * 200.0 / 10000.0);
    }

    return float4(color, 1.0);
}

