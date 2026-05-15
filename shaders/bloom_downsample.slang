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
half karisWeight(half3 col) {
	half lum = dot(col, half3(0.2126h, 0.7152h, 0.0722h));
	return 1.0h / (1.0h + lum);
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
    half3 a = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(-2*x, 2*y)).rgb);
    half3 b = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(0, 2*y)).rgb);
    half3 c = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(2*x, 2*y)).rgb);

    half3 d = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(-x, y)).rgb);
    half3 e = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(x, y)).rgb);

    half3 f = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(-2*x, 0)).rgb);
    half3 g = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(0, 0)).rgb);
    half3 h = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(2*x, 0)).rgb);

    half3 i = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(-x, -y)).rgb);
    half3 j = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(x, -y)).rgb);

    half3 k = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(-2*x, -2*y)).rgb);
    half3 l = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(0, -2*y)).rgb);
    half3 m = (half3)sanitizeColor(input_texture.Sample(input.uv + float2(2*x, -2*y)).rgb);

    half3 downsample;
    if (push.is_first_pass == 1) {
        // Weighted average per group of 4 pixels to suppress fireflies
        // without losing total scene energy ratio.
        half3 groups[5];
        half w[5];

		groups[0] = (a + b + g + f) * 0.25h;
        groups[1] = (b + c + h + g) * 0.25h;
        groups[2] = (f + g + l + k) * 0.25h;
        groups[3] = (g + h + m + l) * 0.25h;
        groups[4] = (d + e + j + i) * 0.25h;

		w[0] = karisWeight(groups[0]);
		w[1] = karisWeight(groups[1]);
		w[2] = karisWeight(groups[2]);
		w[3] = karisWeight(groups[3]);
		w[4] = karisWeight(groups[4]);

        downsample = (groups[0]*w[0] + groups[1]*w[1] + groups[2]*w[2] + groups[3]*w[3] + groups[4]*w[4]);
        downsample /= (w[0] + w[1] + w[2] + w[3] + w[4]);

    } else {
        half3 groups[5];
        groups[0] = (a + b + g + f) * 0.25h;
        groups[1] = (b + c + h + g) * 0.25h;
        groups[2] = (f + g + l + k) * 0.25h;
        groups[3] = (g + h + m + l) * 0.25h;
        groups[4] = (d + e + j + i) * 0.25h;
        downsample = groups[0] * 0.125h + groups[1] * 0.125h + groups[2] * 0.125h + groups[3] * 0.125h + groups[4] * 0.5h;
    }

    return float4(downsample, 1.0);
}
