
struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_color : COLOR;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
	float3 offset;
	float3 light_direction;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	float4x4 transform; // per-object  transform (transl, rotat, scale)
	float4x4 normal_transform;
	float has_texture;
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
    float4 pos : SV_Position;
	float3 frag_color;
	float2 frag_tex_coord;
	float3 frag_light_intensity;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput output;

	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	pos = mul(push_constants.transform, pos); // apply per-object transform
	pos += float4(ubo.offset, 0.0f); // global translation

	float3 normal_world = normalize(mul((float3x3)push_constants.normal_transform, input.in_normal));
	float ambient = 0.1;
	float light_intensity = max(dot(normal_world, ubo.light_direction), 0) + ambient;

    output.pos = mul(ubo.proj, mul(ubo.view, pos)); // view and projection
    output.frag_color = input.in_color;
	output.frag_tex_coord = input.in_tex_coord;
	output.frag_light_intensity = light_intensity;
    return output;
}

[vk::binding(0, 1)]
Sampler2D texture;

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
	if (push_constants.has_texture < 0.5f) {
		return float4(in_vert.frag_color, 1.0f) * float4(in_vert.frag_light_intensity, 1.0f);
	}
	float4 c = texture.Sample(in_vert.frag_tex_coord);
	// Discard fully transparent texels so they don't write depth or color
	if (c.a <= 0.001)
		discard;
	return c;
}
