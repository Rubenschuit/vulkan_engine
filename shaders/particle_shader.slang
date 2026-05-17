#include "particle_common.slangh"

struct UniformBuffer {
	float4x4 view;
	float4x4 proj;
	float4x4 projection_view;
	float4 camera_position;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> g_ubo;

struct VertexInput {
	float4 pos : POSITION;            // xyz = center, w = size
	float4 color : COLOR;             // rgba
	float4 tex_coords : TEXCOORD;     // uv1 and uv2
	uint   atlas_index : TEXCOORD3;
	float  blend : TEXCOORD4;
};

struct VertexOutput {
	float4 pos : SV_Position;
	float4 color : COLOR0;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float2 uv2 : TEXCOORD2;
	nointerpolation uint atlas_idx : TEXCOORD3;
	nointerpolation float blend : TEXCOORD4;
};

static const float2 quad[6] = {
	float2( 0.5,  0.5),
	float2(-0.5, -0.5),
	float2(-0.5,  0.5),

	float2( 0.5, -0.5),
	float2(-0.5, -0.5),
	float2( 0.5,  0.5)
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input, uint corner : SV_VertexID) {

	float4 pos = float4(input.pos.xyz, 1.0);
	float4 clip = mul(g_ubo.projection_view, pos);

	float scale = input.pos.w;
	float2 offset = quad[corner] * scale;

	// Apply camera-facing billboarding offset in clip space
	clip.x += offset.x * g_ubo.proj[0][0];
	clip.y += offset.y * g_ubo.proj[1][1];

	VertexOutput out;
	out.pos = clip;
	out.color = input.color;

	float2 uv = quad[corner] + 0.5f;
	uv.y = 1.0f - uv.y;
	uv /= 8.0f;

	out.uv0 = quad[corner];
	out.uv1 = uv + input.tex_coords.xy;
	out.uv2 = uv + input.tex_coords.zw;
	out.atlas_idx = input.atlas_index;
	out.blend = input.blend;
	return out;
}

[vk::binding(0, 1)]
Sampler2D particle_atlases[16];

[shader("fragment")]
[earlydepthstencil]
float4 fragMain(VertexOutput in_vert) : SV_Target {
	// Sentinel: skip texture sample, render round mask.
	if (in_vert.atlas_idx == ROUND_MASK_SENTINEL) {
		float dot = dot(in_vert.uv0, in_vert.uv0);
		float bloom_intensity = 1.0f - smoothstep(0.0f, 0.1f, dot);
		float brightness = lerp(1.0f, 2.5f, bloom_intensity);
		float edge_alpha = 1.0f - smoothstep(0.01f, 0.25f, dot);
		float alpha = in_vert.color.a * edge_alpha;
		if (alpha <= 0.001f)
			discard;
		return float4(in_vert.color.rgb * brightness * alpha, alpha);
	}

	// lerp between two flipbook frames.
	float4 col1 = particle_atlases[in_vert.atlas_idx].Sample(in_vert.uv1);
	float4 col2 = particle_atlases[in_vert.atlas_idx].Sample(in_vert.uv2);
	float4 col = lerp(col1, col2, in_vert.blend);
	if (col.a <= 0.01f)
		discard;

	float4 final_col = col * in_vert.color;
	return float4(final_col.rgb * final_col.a, final_col.a);
}