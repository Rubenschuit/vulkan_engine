
struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_color : COLOR;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
	float4 ambient_light_color;
	float4 light_color;
	float3 light_position;
};
[vk::binding(0, 0)] // binding 0, set 0
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
    float3 frag_pos_world;
	float3 frag_normal_world;
	float3 frag_color;
	float2 frag_tex_coord;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput output;

	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	float4 world_pos = mul(push_constants.transform, pos); // apply per-object transform

	output.pos = mul(ubo.proj, mul(ubo.view, world_pos)); // view and projection
	output.frag_pos_world = world_pos.xyz;
	output.frag_normal_world = normalize(mul((float3x3)push_constants.normal_transform, input.in_normal));
    output.frag_color = input.in_color;
	output.frag_tex_coord = input.in_tex_coord;
    return output;
}

[vk::binding(0, 1)] // binding 0, set 1
Sampler2D texture;

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
	if (push_constants.has_texture > 0.5f) {
		float4 c = texture.Sample(in_vert.frag_tex_coord);
		// Discard fully transparent texels so they don't write depth or color
		if (c.a <= 0.001)
			discard;
		return c;
	}

	// Light calculations

	float3 light_dir = ubo.light_position - in_vert.frag_pos_world; // normalised later
	float attenuation = 1.0 / (0.01 * dot(light_dir, light_dir) + 0.1); // avoid darkness
	float3 light_color = ubo.light_color.xyz * ubo.light_color.w * attenuation;
	float3 ambient_light_color = ubo.ambient_light_color.xyz * ubo.ambient_light_color.w;
	float3 diffuse_light = light_color * max(dot(normalize(in_vert.frag_normal_world), normalize(light_dir)), 0);
	float3 light = ambient_light_color + diffuse_light;

	return float4(in_vert.frag_color * light, 1.0f);
}
