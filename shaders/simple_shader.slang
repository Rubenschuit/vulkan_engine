struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_color : COLOR;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
};

struct PointLight {
	float4 position;
	float4 color; // .w = intensity
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
	float4 ambient_light_color;
	PointLight point_lights[100]; // MAX_LIGHTS TODO use specialization constant
	uint32_t num_lights;
};
[vk::binding(0, 0)] // binding 0, set 0
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	// Explicit layout to total exactly 128 bytes
	[vk::offset(0)]   float4x4 transform;   // 64
	[vk::offset(64)]  float4   nrm_col0;    // 16
	[vk::offset(80)]  float4   nrm_col1;    // 16
	[vk::offset(96)]  float4   nrm_col2;    // 16  -> 64+48=112
	[vk::offset(112)] float    has_texture; // 4
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
	// Reconstruct 3x3 normal matrix from explicit columns and apply
	float3 nrm;
	nrm.x = dot(push_constants.nrm_col0.xyz, input.in_normal);
	nrm.y = dot(push_constants.nrm_col1.xyz, input.in_normal);
	nrm.z = dot(push_constants.nrm_col2.xyz, input.in_normal);
	output.frag_normal_world = normalize(nrm);
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
	float3 diffuse_light = ubo.ambient_light_color.xyz * ubo.ambient_light_color.w; // start with ambient light
	float3 normal = normalize(in_vert.frag_normal_world);
	for (uint32_t i = 0; i < ubo.num_lights; i++) {
		float3 light_dir = ubo.point_lights[i].position.xyz - in_vert.frag_pos_world; // normalised later
		float attenuation = 1.0 / (0.01 * dot(light_dir, light_dir) + 0.1); // distance light and viewer
		float3 light_color = ubo.point_lights[i].color.xyz * ubo.point_lights[i].color.w * attenuation;
		diffuse_light += light_color * max(dot(normal, normalize(light_dir)), 0);
	}


	return float4(in_vert.frag_color * diffuse_light, 1.0f);
}
