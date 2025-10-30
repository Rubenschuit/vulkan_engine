#define PI 3.1415926536

struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_color : COLOR;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
	float4 in_tangent : TANGENT; // xyz = tangent, w = bitangent sign
	uint in_material_index : MATERIAL_INDEX;
};

struct PointLight {
	float4 position;
	float4 color; // .w = intensity
};

enum RenderMode {
	BRDF = 0,
	NORMAL_VECTOR = 1,
	TANGENT_VECTOR = 2,
	BITANGENT_VECTOR = 3,
	NORMAL_MAP = 4,
	BRDF_MICROFACET = 5,
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
	float3 camera_position;
	float padding;
	float4 ambient_light_color;
	PointLight point_lights[100]; // MAX_LIGHTS TODO use specialization constant
	uint32_t num_lights;
	uint32_t render_mode; // see RenderMode enum
};
[vk::binding(0, 0)] // binding 0, set 0
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	[vk::offset(0)]   float4x4 transform;      // 64
	[vk::offset(64)]  float4   normal_col0;    // 16
	[vk::offset(80)]  float4   normal_col1;    // 16
	[vk::offset(96)]  float4   normal_col2;    // 16
	[vk::offset(112)] float    has_texture;    // 4
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
	float4 pos : SV_Position;
    float3 frag_pos_world;
	float3 frag_normal_world;
	float3 frag_tangent_world;
	float3 frag_bitangent_world;
	nointerpolation float handedness_sign;
	float3 frag_color;
	float2 frag_tex_coord;
	uint frag_material_index;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput output;

	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	float4 world_pos = mul(push_constants.transform, pos); // apply per-object transform

	output.pos = mul(ubo.proj, mul(ubo.view, world_pos)); // view and projection
	output.frag_pos_world = world_pos.xyz;

	// Transform normal and tangent to world space
	float3 normal;
	float3 tangent;
	tangent.x = dot(push_constants.normal_col0.xyz, input.in_tangent.xyz);
	tangent.y = dot(push_constants.normal_col1.xyz, input.in_tangent.xyz);
	tangent.z = dot(push_constants.normal_col2.xyz, input.in_tangent.xyz);
	normal.x = dot(push_constants.normal_col0.xyz, input.in_normal);
	normal.y = dot(push_constants.normal_col1.xyz, input.in_normal);
	normal.z = dot(push_constants.normal_col2.xyz, input.in_normal);
	normal = normalize(normal);

	// Compute bitangent as cross product of normal and tangent with proper handedness ( *-1 or *1 )
	float3 bitangent = normalize(cross(normal, tangent) * input.in_tangent.w);

	output.frag_normal_world = normal;
	output.frag_tangent_world = tangent;
	output.frag_bitangent_world = bitangent;
	output.handedness_sign = input.in_tangent.w;
	// pass through
    output.frag_color = input.in_color;
	output.frag_tex_coord = input.in_tex_coord;
	output.frag_material_index = input.in_material_index;

    return output;
}

// binding
[vk::binding(0, 1)]
Sampler2DArray albedo_textures;

[vk::binding(1, 1)]
Sampler2DArray normal_textures;

[vk::binding(2, 1)]
Sampler2DArray metallic_roughness_textures;


// ----------------------------------------------------------------------------
//
// Specular term: a Cook-Torrance specular microfacet model, with a GGX normal
// distribution function, a Smith-GGX height-correlated visibility function,
// and a Schlick Fresnel function.
// Diffuse term: a Lambertian diffuse model.
// ----------------------------------------------------------------------------

// PBR Helper Functions
float3 fresnelSchlick(float cos_theta, float3 F0) {
	return F0 + (1.0 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// Distribution with long-tailed falloff and short peak in the highlights
// Used for specular BRDF (Bidirectional Reflectance Distribution Function)
float distributionGGX(float3 N, float3 H, float roughness) {
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = clamp(dot(N, H), 0.0, 1.0);
	float NdotH2 = NdotH * NdotH;

	float num = a2;
	float denom = (NdotH2 * (a2 - 1.0) + 1.0);
	denom = PI * denom * denom;

	return num / denom;
}

float geometrySchlickGGX(float NdotV, float roughness) {
	float r = (roughness + 1.0);
	float k = (r * r) / 8.0;

	float num = NdotV;
	float denom = NdotV * (1.0 - k) + k;

	return num / denom;
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
	float ggx2 = geometrySchlickGGX(NdotV, roughness);
	float ggx1 = geometrySchlickGGX(NdotL, roughness);

	return ggx1 * ggx2;
}

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
	float4 c;

	// PBR Lighting for textured objects
	// using the BRDF microfacet model
	if (push_constants.has_texture > 0.5f) {

		float3 tex_coord = float3(in_vert.frag_tex_coord, in_vert.frag_material_index);
		float4 albedo = albedo_textures.Sample(tex_coord);

		if (albedo.a <= 0.001)
			discard;

		float4 mr = metallic_roughness_textures.Sample(tex_coord);
		float4 normal_map = normal_textures.Sample(tex_coord);

		// Extract PBR properties
		float metallic = mr.b;
		float roughness = mr.g;

		float3 N = normalize(in_vert.frag_normal_world);
		float3 T = normalize(in_vert.frag_tangent_world);
		float3 B = normalize(in_vert.frag_bitangent_world);
		float3 world_normal = N;
		if (ubo.render_mode == RenderMode::BRDF_MICROFACET || ubo.render_mode == RenderMode::NORMAL_MAP) { // BRDF Microfacet (use normal map)
			// Transform sampled normal from tangent space to world space
			float3 tangent_space_normal = normalize(normal_map.xyz * 2.0 - float3(1.0, 1.0, 1.0)); // Convert from [0,1] to [-1,1]
			float3x3 TBN = transpose(float3x3(T, B, N));
			world_normal = normalize(mul(TBN, tangent_space_normal));
		}

		if (ubo.render_mode == RenderMode::NORMAL_VECTOR) // normal vector
			return float4(0.5 * N + 0.5, 1.0f);
		else if (ubo.render_mode == RenderMode::TANGENT_VECTOR) // tangent vector
			return float4(0.5 * T + 0.5, 1.0f);
		else if (ubo.render_mode == RenderMode::BITANGENT_VECTOR) // bitangent vector
			return float4(0.5 * B + 0.5, 1.0f);
		else if (ubo.render_mode == RenderMode::NORMAL_MAP) // sampled normal
			return float4(0.5 * world_normal + 0.5, 1.0f);

		// View direction
		float3 view_dir = normalize(ubo.camera_position - in_vert.frag_pos_world);

		// Calculate Fresnel F0
		float3 F0 = lerp(float3(0.04), albedo.rgb, metallic);

		// Accumulate light contribution
		float3 Lo = float3(0.0);

		// For every light in the current scene
		for (uint32_t i = 0; i < ubo.num_lights; i++) {
			float3 light_dir = ubo.point_lights[i].position.xyz - in_vert.frag_pos_world;
			float3 L = normalize(light_dir);

			// Cook-Torrance BRDF
			float NdotL = clamp(dot(world_normal, L), 0.0, 1.0);

			// Early exit if surface is facing away from light
			if (NdotL <= 0.0)
				continue;

			float NdotV = clamp(dot(world_normal, view_dir), 0.0, 1.0);
			float3 H = normalize(view_dir + L);

			// Calculate attenuation and radiance (intensity of the light)
			float distance_sq = dot(light_dir, light_dir);
			float attenuation = 1.0 / (0.01 * distance_sq + 0.1);
			float3 radiance = ubo.point_lights[i].color.xyz * ubo.point_lights[i].color.w * attenuation;

			// Calculate Fresnel
			float3 F = fresnelSchlick(max(dot(H, view_dir), 0.0), F0);

			// Calculate Cook-Torrance specular
			float D = distributionGGX(world_normal, H, roughness);
			float G = geometrySmith(NdotV, NdotL, roughness);

			float3 numerator = D * G * F;
			float denominator = 4.0 * NdotV * NdotL;
			float3 specular = numerator / max(denominator, 0.00001);

			// Energy conservation through diffuse coefficient
			float3 kD = float3(1.0, 1.0, 1.0) - F;
			kD *= 1.0 - metallic;

			// Add together the diffuse and specular
			Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL;
		}

		// Ambient lighting
		float3 ambient = ubo.ambient_light_color.xyz * ubo.ambient_light_color.w * albedo.rgb;

		// Final color
		c.rgb = Lo + ambient;
		c.a = albedo.a;

	} else {
		// Simple diffuse lighting for non-textured objects
		float4 albedo = float4(in_vert.frag_color, 1.0f);
		float3 diffuse_light = ubo.ambient_light_color.xyz * ubo.ambient_light_color.w; // start with ambient light
		float3 normal = normalize(in_vert.frag_normal_world);

		for (uint32_t i = 0; i < ubo.num_lights; i++) {
			float3 light_dir = ubo.point_lights[i].position.xyz - in_vert.frag_pos_world; // normalised later
			float attenuation = 1.0 / (0.01 * dot(light_dir, light_dir) + 0.1); // distance light and viewer
			float3 light_color = ubo.point_lights[i].color.xyz * ubo.point_lights[i].color.w * attenuation;
			diffuse_light += light_color * max(dot(normal, normalize(light_dir)), 0);
		}

		//apply lighting
		c.rgb = albedo.rgb * diffuse_light;
		c.a = 1.0f;
	}

	return c;
}
