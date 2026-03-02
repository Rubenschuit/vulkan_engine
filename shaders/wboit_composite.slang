// WBOIT composite pass: reads accumulation and revealage textures,
// reconstructs the average transparent color, and outputs it for blending
// over the opaque scene.

[vk::binding(0, 0)]
Sampler2D accum_texture;
[vk::binding(1, 0)]
Sampler2D revealage_texture;

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
	float4 accum = accum_texture.Sample(input.uv);
	float revealage = revealage_texture.Sample(input.uv).r;

	// No transparency was rendered at this pixel
	if (revealage >= 1.0 - 1e-4)
		discard;

	// Reconstruct average color
	float3 avg_color = accum.rgb / max(accum.a, 1e-5);

	// Output with alpha = coverage for over-blending onto the opaque scene
	return float4(avg_color, 1.0 - revealage);
}
