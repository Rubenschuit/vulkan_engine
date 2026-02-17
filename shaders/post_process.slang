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
    int tone_map_mode; // 0=None, 1=Reinhard, 2=ACES Filmic, 3=PBR Neutral, 4=GT Tonemap
    float hdr_peak_white;
    float padding;
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

// ACES fitted tone mapping (Stephen Hill fit)
// sRGB → XYZ → D65_2_D60 → AP1 → RRT_SAT
static const float3x3 ACESInputMat = {
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777
};
// ODT_SAT → XYZ → D60_2_D65 → sRGB
static const float3x3 ACESOutputMat = {
     1.60475, -0.53108, -0.07367,
    -0.10208,  1.10813, -0.00605,
    -0.00327, -0.07276,  1.07602
};
float3 RRTAndODTFit(float3 v) {
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}
float3 ACESFitted(float3 color) {
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
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

// Khronos PBR Neutral tone mapping (SDR, preserves PBR material colors)
// https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/pbrNeutral.glsl
float3 PBRNeutralToneMapping(float3 color) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression)
		return color;
    const float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return lerp(color, float3(new_peak), g);
}

// GT/Uchimura tone mapping (Hajime Uchimura, Polyphony Digital)
// P = peak brightness: 1.0 for SDR, >1.0 for HDR
// https://www.desmos.com/calculator/gslcdxvipg
float3 GTTonemap(float3 x, float P) {
    const float a = 1.0;  // contrast
    const float m = 0.22; // linear section start
    const float l = 0.4;  // linear section length
    const float c = 1.33; // black (toe strength)
    const float b = 0.0;  // pedestal

    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;

    float3 w0 = 1.0 - smoothstep(0.0, m, x);
    float3 w2 = step(m + l0, x);
    float3 w1 = 1.0 - w0 - w2;

    float3 T = m * pow(x / m, c) + b;                     // toe
    float3 L = m + a * (x - m);                           // linear section
    float3 S = P - (P - S1) * exp(CP * (x - S0));         // shoulder → P

    return T * w0 + L * w1 + S * w2;
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

    // Tone mapping
    // Reinhard, ACES, and PBR Neutral are SDR-only (output clamped to [0,1]);
    // fall back to GT tonemap in HDR modes to preserve headroom.
    bool is_hdr = push.color_space != 0;
    if (push.tone_map_mode == 1)
        color = is_hdr ? GTTonemap(color, push.hdr_peak_white) : color / (color + float3(1.0)); // Reinhard (SDR) / GT (HDR)
    else if (push.tone_map_mode == 2)
        color = is_hdr ? GTTonemap(color, push.hdr_peak_white) : ACESFitted(color);    // ACES (SDR) / GT (HDR)
    else if (push.tone_map_mode == 3)
        color = is_hdr ? GTTonemap(color, push.hdr_peak_white) : PBRNeutralToneMapping(color); // PBR Neutral (SDR) / GT (HDR)
    else if (push.tone_map_mode == 4)
        color = GTTonemap(color, is_hdr ? push.hdr_peak_white : 1.0);                  // GT Tonemap

    // Color space output
    if (push.color_space == 1) {
        // HDR (Extended SRGB Linear) — already tone mapped above
    } else if (push.color_space == 2) {
        // HDR (HDR10 ST2084 / PQ)
        color = bt709to2020(color);
        color = linearToPQ(color * 200.0 / 10000.0);
    }

    return float4(color, 1.0);
}

