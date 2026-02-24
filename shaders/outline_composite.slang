// Outline composite: reads JFA distance field + mask, draws anti-aliased outline
// via alpha blending onto the post-processed scene image.

[vk::binding(0, 0)]
Sampler2D<int2> jfa_texture;
[vk::binding(1, 0)]
Sampler2D mask_texture;

struct OutlinePush {
	float2 image_size;
	float outline_width;
	float _pad;
	float4 outline_color;
};
[[vk::push_constant]] OutlinePush push;

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
	int2 coord = int2(input.uv * push.image_size);
	coord = clamp(coord, int2(0), int2(push.image_size) - 1);

	float mask = mask_texture.Load(int3(coord, 0)).r;

	int2 seed = jfa_texture.Load(int3(coord, 0));
	if (seed.x < 0)
		discard;

	float2 diff = float2(coord - seed);
	float dist = sqrt(dot(diff, diff));

	// Outline: outside the mask but within outline_width of a seed
	if (mask < 0.5 && dist <= push.outline_width) {
		float alpha = saturate(push.outline_width - dist + 0.5);
		return float4(push.outline_color.rgb, push.outline_color.a * alpha);
	}

	discard;
	return float4(0, 0, 0, 0);
}