#define PI 3.1415926536

struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_color : COLOR;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
	float4 in_tangent : TANGENT; // xyz = tangent, w = bitangent sign
};

struct PointLight {
	float4 position;
	float4 color; // .w = intensity
};

struct ShadowLight {
	float4x4 light_view;
	float4x4 light_proj;
	float4x4 shadow_matrix;
	float4 light_index_padding; // x = light_index, yzw = padding
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
	float4x4 projection_view;
	float4 camera_position; // .w unused
	float4 ambient_light_color;
	PointLight point_lights[40]; // constant must be equal to ve::MAX_LIGHTS, TODO use specialization constant
	ShadowLight shadow_lights[3]; // MAX_SHADOW_LIGHTS
	uint32_t num_lights;
	uint32_t num_shadow_lights;
	uint32_t render_mode; // see RenderMode enum
	uint32_t shadow_mode;
	float shadow_bias;
};
[vk::binding(0, 0)] // binding 0, set 0
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	[vk::offset(0)]   float4x4 transform;
	[vk::offset(64)]  float4   normal_col0;
	[vk::offset(80)]  float4   normal_col1;
	[vk::offset(96)]  float4   normal_col2;
	[vk::offset(112)] float    has_texture;
	[vk::offset(116)] float    alpha_cutoff;
	[vk::offset(120)] uint32_t material_flags;  // bits 0-1: alpha_mode, bit 2: double_sided, bit 3: flip_tex_coord_v, bit 4: use_spec_gloss_texture
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
	float4 shadow_coord[3]; // MAX_SHADOW_LIGHTS
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

	output.pos = mul(ubo.projection_view, world_pos);
	output.frag_pos_world = world_pos.xyz;

	// Transform normal and tangent to world space
	float3x3 normal_matrix = float3x3(
		float3(push_constants.normal_col0.x, push_constants.normal_col1.x, push_constants.normal_col2.x),
		float3(push_constants.normal_col0.y, push_constants.normal_col1.y, push_constants.normal_col2.y),
		float3(push_constants.normal_col0.z, push_constants.normal_col1.z, push_constants.normal_col2.z)
	);
	float3 tangent = mul(normal_matrix, input.in_tangent.xyz);
	float3 normal = normalize(mul(normal_matrix, input.in_normal));

	// Compute bitangent as cross product of normal and tangent with proper handedness ( *-1 or *1 )
	float3 bitangent = normalize(cross(normal, tangent) * input.in_tangent.w);

	output.frag_normal_world = normal;
	output.frag_tangent_world = tangent;
	output.frag_bitangent_world = bitangent;
	// pass through
    output.frag_color = input.in_color;
	float2 tex_coord = input.in_tex_coord;
	if ((push_constants.material_flags & 8u) != 0u)  // flip_tex_coord_v (bit 3)
		tex_coord.y = 1.0f - tex_coord.y;
	output.frag_tex_coord = tex_coord;

    // calculate shadow coordinates only if shadows are enabled
	if (ubo.shadow_mode != ShadowMode::DISABLED) {
		for (uint32_t i = 0; i < ubo.num_shadow_lights; i++) {
			output.shadow_coord[i] = mul(ubo.shadow_lights[i].shadow_matrix, world_pos);
		}
	}

    return output;
}

// Per-material textures (set 1)
[vk::binding(0, 1)]
Sampler2D albedo_texture;

[vk::binding(1, 1)]
Sampler2D normal_texture;

[vk::binding(2, 1)]
Sampler2D metallic_roughness_texture;

[vk::binding(3, 1)]
Sampler2D occlusion_texture;

[vk::binding(4, 1)]
Sampler2D emissive_texture;

// Material PBR data
struct MaterialConstants {
	float4 base_color_factor;
	float4 emissive_pad;   // xyz = emissive_factor, w = pad
	float4 params;         // x = metallic, y = roughness, z = emissive_strength, w = transmission
	float4 extra;          // xyz = dielectric F0 (KHR_materials_pbrSpecularGlossiness.specularFactor or IOR-derived), w = ior
};
[vk::binding(5, 1)]
ConstantBuffer<MaterialConstants> material_constants;

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
	float scale = 1.0f;
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

	float2 tex_coord = in_vert.frag_tex_coord;
	float4 albedo = albedo_texture.Sample(tex_coord);

	// glTF alphaMode: OPAQUE(0)=no discard, MASK(1)=discard below cutoff, BLEND(2)=no discard
	uint32_t alpha_mode = push_constants.material_flags & 3u;
	if (alpha_mode == 1 && albedo.a < push_constants.alpha_cutoff)
		discard;

	float4 mr = metallic_roughness_texture.Sample(tex_coord);
	float4 normal_map = normal_texture.Sample(tex_coord);
	float4 occlusion_sample = occlusion_texture.Sample(tex_coord);
	float3 emissive_sample = emissive_texture.Sample(tex_coord).rgb;

	// Apply material factors from glTF (baseColorFactor, metallicFactor, roughnessFactor, emissiveFactor, emissiveStrength)
	albedo *= material_constants.base_color_factor;
	float metallic;
	float roughness;
	float3 F0;
	if ((push_constants.material_flags & 16u) != 0u) {
		// Spec-gloss texture
		metallic = 0.0;
		roughness = (1.0 - mr.a) * material_constants.params.y;
		F0 = material_constants.extra.xyz * mr.rgb;
	} else {
		// Metallic-roughness texture
		metallic = mr.b * material_constants.params.x;
		roughness = mr.g * material_constants.params.y;
		F0 = lerp(material_constants.extra.xyz, albedo.rgb, metallic);
	}
	float transmission = material_constants.params.w;
	emissive_sample = emissive_sample * material_constants.emissive_pad.xyz * material_constants.params.z;

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

	// Create mapping from light index to shadow term (defaults to 1.0 = no shadow)
	float shadow_terms[20]; // MAX_LIGHTS
	for (uint32_t i = 0; i < ubo.num_lights; i++) {
		shadow_terms[i] = 1.0;
	}

	// Calculate shadows for shadow-casting lights
	if (ubo.shadow_mode != ShadowMode::DISABLED) {
		for (uint32_t shadow_idx = 0; shadow_idx < ubo.num_shadow_lights; shadow_idx++) {
			uint32_t light_idx = uint32_t(ubo.shadow_lights[shadow_idx].light_index_padding.x);

			if (ubo.shadow_mode == ShadowMode::REGULAR) {
				shadow_terms[light_idx] = calculateShadowTerm(shadow_idx,
					in_vert.shadow_coord[shadow_idx] / in_vert.shadow_coord[shadow_idx].w, float2(0.0, 0.0));
			}
			else if (ubo.shadow_mode == ShadowMode::PCF) {
				shadow_terms[light_idx] = calculateShadowPCF(shadow_idx,
					in_vert.shadow_coord[shadow_idx] / in_vert.shadow_coord[shadow_idx].w);
			}
		}
	}

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
		float attenuation = 1.0 / (distance_sq);
		float3 radiance = ubo.point_lights[i].color.xyz * attenuation; // color already contains intensity

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
		// Glass / high-transmission: reduce diffuse so surface is reflection-dominated, not matte
		kD *= 1.0 - transmission;

		// Add together the diffuse and specular, multiplied by shadow term
		Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL * shadow_terms[i];
	}



	// Ambient lighting (scaled by occlusion: white = full ambient, black = no ambient)
	float3 ambient = ubo.ambient_light_color.xyz * albedo.rgb * occlusion_sample.r;
	// Glass / high-transmission: reduce ambient so it doesn't look flat
	ambient *= 1.0 - transmission;

	// Final color: lit + ambient + emissive
	c.rgb = Lo + ambient + emissive_sample;
	// High-transmission materials: enforce minimum alpha so glass/windows/bottles stay clearly visible.
	// Threshold must match HIGH_TRANSMISSION_THRESHOLD in pbr_render_system.cpp.
	static const float HIGH_TRANSMISSION_THRESHOLD = 0.5;
	float raw_alpha = (1.0 - transmission) * albedo.a;
	c.a = (transmission > HIGH_TRANSMISSION_THRESHOLD) ? max(raw_alpha, 0.45) : raw_alpha;

	return c;
}

