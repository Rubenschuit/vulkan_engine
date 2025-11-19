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
	float4x4 light_view;
	float4x4 light_proj;
};

// Reminder: keep this in sync with the RenderMode enum in ve_frame_info.hpp
enum RenderMode {
	BRDF = 0,
	NORMAL_VECTOR = 1,
	TANGENT_VECTOR = 2,
	BITANGENT_VECTOR = 3,
	NORMAL_MAP = 4,
	BRDF_MICROFACET = 5,
};

enum ShadowMode {
	DISABLED = 0,
	REGULAR = 1,
	PCF = 2,
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
	float4 camera_position; // .w unused
	float4 ambient_light_color;
	PointLight point_lights[20]; // constant must be equal to ve::MAX_LIGHTS, TODO use specialization constant
	uint32_t num_lights;
	uint32_t render_mode; // see RenderMode enum
	uint32_t shadow_mode;
	float shadow_bias;
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
	float3 frag_color;
	float2 frag_tex_coord;
	float4 shadow_coord[7]; // TODO use specialization constant
	uint frag_material_index;
};

// Transform x and y coordinates from [0,1] to [-1,1] and keep z and w coordinates unchanged
static const float4x4 bias_matrix = float4x4(
    0.5, 0.0, 0.0, 0.5,
    0.0, 0.5, 0.0, 0.5,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0
);

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput output;

	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	float4 world_pos = mul(push_constants.transform, pos); // apply per-object transform

	// TODO: precompute view and projection matrices?
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
	// pass through
    output.frag_color = input.in_color;
	output.frag_tex_coord = input.in_tex_coord;
	output.frag_material_index = input.in_material_index;

    // calculate shadow coordinates only if shadows are enabled
	// TODO: precompute bias matrix * light_proj * light_view ?
	if (ubo.shadow_mode != ShadowMode::DISABLED) {
		for (uint32_t i = 0; i < ubo.num_lights; i++) {
			output.shadow_coord[i] = mul(bias_matrix, mul(ubo.point_lights[i].light_proj, mul(ubo.point_lights[i].light_view, mul(push_constants.transform, float4(input.in_pos, 1.0)))));
		}
	}

    return output;
}

// binding
[vk::binding(0, 1)]
Sampler2DArray albedo_textures;

[vk::binding(1, 1)]
Sampler2DArray normal_textures;

[vk::binding(2, 1)]
Sampler2DArray metallic_roughness_textures;

// Shadow maps (set 2) - single 2D array texture
// Note: On MoltenVK, comparison sampler is not available, so we use regular sampler
// and do comparison manually in shader
[vk::binding(0, 2)]
SamplerState shadow_sampler;  // Regular sampler (comparison disabled on macOS)
[vk::binding(1, 2)]
Texture2DArray shadow_maps;  // Single 2D array texture with MAX_LIGHTS layers

// Helper function to calculate shadow term for a given light
float calculateShadowTerm(uint32_t light_index, float4 shadow_coord, float2 offset) {
	float shadow = 1.0;
	if ( shadow_coord.z > -1.0 && shadow_coord.z < 1.0 ) {
        float shadow_depth = shadow_maps.SampleLevel(shadow_sampler, float3(shadow_coord.xy + offset, light_index), 0.0).r;
		if ( shadow_coord.w > 0.0 && shadow_depth + ubo.shadow_bias < shadow_coord.z ) {
			shadow = 0.01f;
		}
	}
	return shadow;
}

// PCF (Percentage Closer Filtering) to reduce shadow artifacts.
// Calculates the average shadow value over a small neighborhood of the shadow map.
float calculateShadowPCF(uint32_t light_index, float4 shadow_coord) {

	float shadow = 0.0;
	int3 texel_size;
	shadow_maps.GetDimensions(texel_size.x, texel_size.y, texel_size.z);
	float scale = 1.5f;
	float dx = scale / texel_size.x;
	float dy = scale / texel_size.y;

	int count = 0;
	int range = 1; // sample shadow with offsets in [-range, range]


	for (int x = -range; x <= range; x++) {
		for (int y = -range; y <= range; y++) {
			shadow += calculateShadowTerm(light_index, shadow_coord, float2(x * dx, y * dy));
			count++;
		}
	}
	return shadow / count;
}

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
	float3 view_dir = normalize(ubo.camera_position.xyz - in_vert.frag_pos_world);

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

		// Calculate shadow term using helper function
		float shadow = 1.0;
		if (ubo.shadow_mode == ShadowMode::DISABLED) {
			shadow = 1.0;
		}
		else if (ubo.shadow_mode == ShadowMode::REGULAR) {
			shadow = calculateShadowTerm(i, in_vert.shadow_coord[i] / in_vert.shadow_coord[i].w, float2(0.0, 0.0));
		}
		else if (ubo.shadow_mode == ShadowMode::PCF) {
			shadow = calculateShadowPCF(i, in_vert.shadow_coord[i] / in_vert.shadow_coord[i].w);
		}

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

		// Add together the diffuse and specular, multiplied by shadow term
		Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL * shadow;
	}

	// Ambient lighting
	float3 ambient = ubo.ambient_light_color.xyz * ubo.ambient_light_color.w * albedo.rgb;

	// Final color
	c.rgb = Lo + ambient;
	c.a = albedo.a;

	return c;
}

