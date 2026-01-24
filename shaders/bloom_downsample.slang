// This shader implements downsampling with 13 samples around the current pixel.
// The input texture is a HDR image containing the scene

[vk::binding(0, 0)]
Sampler2D input_texture;

struct BloomDownsamplePushConstant {
    int is_first_pass;
};

[vk::push_constant]
BloomDownsamplePushConstant push;

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

// Prevents NaNs, Infs, and negative values from propagating
float3 sanitizeColor(float3 c) {
    float3 result = clamp(c, 0.0, 64.0f);
    if (any(isnan(result)) || any(isinf(result))) return 0.0.xxx;
    return result;
}

// Returns a weight inversely proportional to luminance to suppress fireflies
float karisWeight(float3 col) {
	// Convert color to luminance
	float lum = dot(col, float3(0.2126, 0.7152, 0.0722));
	return 1.0 / (1.0 + lum);
}

[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_Target {
    uint width, height;
    input_texture.GetDimensions(width, height);
    float2 texel_size = 1.0 / float2(width, height);
    float x = texel_size.x;
    float y = texel_size.y;

    // Take 13 samples around current texel:
    // a - b - c
    // - d - e -
    // f - g - h
    // - i - j -
    // k - l - m
    float3 a = sanitizeColor(input_texture.Sample(input.uv + float2(-2*x, 2*y)).rgb);
    float3 b = sanitizeColor(input_texture.Sample(input.uv + float2(0, 2*y)).rgb);
    float3 c = sanitizeColor(input_texture.Sample(input.uv + float2(2*x, 2*y)).rgb);

    float3 d = sanitizeColor(input_texture.Sample(input.uv + float2(-x, y)).rgb);
    float3 e = sanitizeColor(input_texture.Sample(input.uv + float2(x, y)).rgb);

    float3 f = sanitizeColor(input_texture.Sample(input.uv + float2(-2*x, 0)).rgb);
    float3 g = sanitizeColor(input_texture.Sample(input.uv + float2(0, 0)).rgb);
    float3 h = sanitizeColor(input_texture.Sample(input.uv + float2(2*x, 0)).rgb);

    float3 i = sanitizeColor(input_texture.Sample(input.uv + float2(-x, -y)).rgb);
    float3 j = sanitizeColor(input_texture.Sample(input.uv + float2(x, -y)).rgb);

    float3 k = sanitizeColor(input_texture.Sample(input.uv + float2(-2*x, -2*y)).rgb);
    float3 l = sanitizeColor(input_texture.Sample(input.uv + float2(0, -2*y)).rgb);
    float3 m = sanitizeColor(input_texture.Sample(input.uv + float2(2*x, -2*y)).rgb);

    float3 downsample;
    if (push.is_first_pass == 1) {
        // Weighted average per group of 4 pixels to suppress fireflies
        // without losing total scene energy ratio.
        float3 groups[5];
        float w[5];

		groups[0] = (a + b + g + f) * 0.25;
        groups[1] = (b + c + h + g) * 0.25;
        groups[2] = (f + g + l + k) * 0.25;
        groups[3] = (g + h + m + l) * 0.25;
        groups[4] = (d + e + j + i) * 0.25;

		w[0] = karisWeight(groups[0]);
		w[1] = karisWeight(groups[1]);
		w[2] = karisWeight(groups[2]);
		w[3] = karisWeight(groups[3]);
		w[4] = karisWeight(groups[4]);

        downsample = (groups[0]*w[0] + groups[1]*w[1] + groups[2]*w[2] + groups[3]*w[3] + groups[4]*w[4]);
        downsample /= (w[0] + w[1] + w[2] + w[3] + w[4]);

    } else {
        float3 groups[5];
        groups[0] = (a + b + g + f) * 0.25;
        groups[1] = (b + c + h + g) * 0.25;
        groups[2] = (f + g + l + k) * 0.25;
        groups[3] = (g + h + m + l) * 0.25;
        groups[4] = (d + e + j + i) * 0.25;
        downsample = groups[0] * 0.125 + groups[1] * 0.125 + groups[2] * 0.125 + groups[3] * 0.125 + groups[4] * 0.5;
    }

    return float4(downsample, 1.0);
}
