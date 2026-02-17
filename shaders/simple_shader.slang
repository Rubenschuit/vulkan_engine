#define PI 3.1415926536
#define MAX_LIGHTS 160
#define MAX_SHADOW_LIGHTS 3
#define MAX_DIR_LIGHTS 4

struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
};

struct PointLight {
	float4 position;
	float4 color; // .w = intensity
};

struct DirectionalLight {
	float4 direction;  // xyz = direction toward surface, w = unused
	float4 color;      // xyz = color * intensity, w = intensity
};

struct ShadowLight {
	float4x4 light_view;
	float4x4 light_proj;
	float4x4 shadow_matrix;
	float4 light_index_padding; // x = light_index, y = type (0=point, 1=directional), zw = padding
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
	float4 camera_position;
	float4 ambient_light_color;
	PointLight point_lights[MAX_LIGHTS]; // MAX_LIGHTS TODO use specialization constant
	ShadowLight shadow_lights[MAX_SHADOW_LIGHTS]; // MAX_SHADOW_LIGHTS
	uint32_t num_lights;
	uint32_t num_shadow_lights;
	uint32_t render_mode; // see RenderMode enum
	uint32_t shadow_mode;
	float shadow_bias;
	uint32_t num_dir_lights;
	float _pad_dir_0;
	float _pad_dir_1;
	DirectionalLight dir_lights[MAX_DIR_LIGHTS];
};
[vk::binding(0, 0)] // binding 0, set 0
ConstantBuffer<UniformBuffer> ubo;

// Per-instance transforms stored in SSBO, indexed by SV_InstanceID (always 0-based)
struct InstanceData {
	float4x4 transform;
	float4   normal_col0;
	float4   normal_col1;
	float4   normal_col2;
};
[vk::binding(1, 0)] // binding 1, set 0
StructuredBuffer<InstanceData> instances;

struct PushConstantData {
	[vk::offset(0)] float    has_texture;       // 4
	[vk::offset(4)] uint32_t instance_offset;   // 4 (SSBO offset, SV_InstanceID is 0-based in Slang)
	[vk::offset(8)] float    padding[2];        // 8 (pad to 16)
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
	float4 pos : SV_Position;
    float3 frag_pos_world;
	float3 frag_normal_world;
	float2 frag_tex_coord;
	float4 shadow_coord[MAX_SHADOW_LIGHTS]; // MAX_SHADOW_LIGHTS
};

static const float4x4 bias_matrix = float4x4(
    0.5, 0.0, 0.0, 0.5,
    0.0, 0.5, 0.0, 0.5,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0);

[shader("vertex")]
VertexOutput vertMain(VertexInput input, uint instance_id : SV_InstanceID) {
    VertexOutput output;

	InstanceData inst = instances[instance_id + push_constants.instance_offset];

	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	float4 world_pos = mul(inst.transform, pos); // apply per-instance transform

	output.pos = mul(ubo.projection_view, world_pos); // pre-computed view and projection
	output.frag_pos_world = world_pos.xyz;

	// Transform normal to world space
	output.frag_normal_world = normalize(float3(
		dot(inst.normal_col0.xyz, input.in_normal),
		dot(inst.normal_col1.xyz, input.in_normal),
		dot(inst.normal_col2.xyz, input.in_normal)
	));
	output.frag_tex_coord = input.in_tex_coord;

	 // calculate shadow coordinates only if shadows are enabled
	if (ubo.shadow_mode != 0) {
		for (uint32_t i = 0; i < ubo.num_shadow_lights; i++) {
			output.shadow_coord[i] = mul(ubo.shadow_lights[i].shadow_matrix, world_pos);
		}
	}
    return output;
}

[vk::binding(0, 1)]
Sampler2D albedo_texture;

// Shadow maps (set 2) - single 2D array texture
// Note: On MoltenVK, comparison sampler is not available, so we use regular sampler
// and do comparison manually in shader
[vk::binding(0, 2)]
SamplerState shadow_sampler;  // Regular sampler (comparison disabled on macOS)
[vk::binding(1, 2)]
Texture2DArray shadow_maps;  // Single 2D array texture with MAX_LIGHTS layers


float calculateShadowTerm(uint32_t light_index, float4 shadow_coord, float2 offset) {
	float bias = ubo.shadow_bias;
	float shadow = 1.0;
	if ( shadow_coord.z > -1.0 && shadow_coord.z < 1.0 ) {
        float shadow_depth = shadow_maps.SampleLevel(shadow_sampler, float3(shadow_coord.xy + offset, light_index), 0.0).r;
		if ( shadow_coord.w > 0.0 && shadow_depth + bias < shadow_coord.z ) {
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
	float scale = 0.5f;
	float dx = scale / texel_size.x;
	float dy = scale / texel_size.y;

	int count = 0;
	int range = 2; // sample shadow with offsets in [-range, range]


	for (int x = -range; x <= range; x++) {
		for (int y = -range; y <= range; y++) {
			shadow += calculateShadowTerm(light_index, shadow_coord, float2(x * dx, y * dy));
			count++;
		}
	}
	return shadow / count;
}

[shader("fragment")]

float4 fragMain(VertexOutput in_vert) : SV_Target {
	/*
	// DEBUG:Visualize shadow map of point light index 1 on textured quad
	if (push_constants.has_texture > 0.5f && ubo.num_lights > 0 && ubo.shadow_mode != 0) {
		float shadow_depth = shadow_maps.SampleLevel(shadow_sampler, float3(in_vert.frag_tex_coord, 0), 0.0).r;
		// Simple grayscale visualization: black=near, white=far

		// amplify the darker values
		float amplified = shadow_depth;
		if (shadow_depth < 0.99f) {
			amplified = shadow_depth * 0.2f;
		}

		return float4(amplified, amplified, amplified, 1.0);
	}
    */
	float3 normal = normalize(in_vert.frag_normal_world);

	// visualize normal vector
	if (ubo.render_mode == RenderMode::NORMAL_VECTOR)
		return float4(0.5 * normal + 0.5, 1.0f);

	// Base color for untextured objects
	float4 base_color = float4(1.0, 1.0, 1.0, 1.0);
	if (push_constants.has_texture > 0.5f) {
		float2 uv = in_vert.frag_tex_coord;
		uv.x = 1.0f - uv.x;
		//uv.y = 1.0f - uv.y;
		base_color = albedo_texture.Sample(uv);
		if (base_color.a <= 0.001f)
			discard;
		return base_color;
	}

	// Shadow terms for point lights and directional lights (default 1.0 = no shadow)
	float point_shadow_terms[MAX_LIGHTS];
	float dir_shadow_terms[MAX_DIR_LIGHTS];
	for (uint32_t i = 0; i < ubo.num_lights; i++) point_shadow_terms[i] = 1.0;
	for (uint32_t i = 0; i < ubo.num_dir_lights; i++) dir_shadow_terms[i] = 1.0;

	// Calculate shadows for shadow-casting lights
	if (ubo.shadow_mode != ShadowMode::DISABLED) {
		for (uint32_t shadow_idx = 0; shadow_idx < ubo.num_shadow_lights; shadow_idx++) {
			uint32_t light_idx = uint32_t(ubo.shadow_lights[shadow_idx].light_index_padding.x);
			float is_directional = ubo.shadow_lights[shadow_idx].light_index_padding.y;

			float shadow;
			if (ubo.shadow_mode == ShadowMode::REGULAR) {
				shadow = calculateShadowTerm(shadow_idx,
					in_vert.shadow_coord[shadow_idx] / in_vert.shadow_coord[shadow_idx].w, float2(0.0, 0.0));
			} else {
				shadow = calculateShadowPCF(shadow_idx,
					in_vert.shadow_coord[shadow_idx] / in_vert.shadow_coord[shadow_idx].w);
			}

			if (is_directional > 0.5)
				dir_shadow_terms[light_idx] = shadow;
			else
				point_shadow_terms[light_idx] = shadow;
		}
	}

	// Apply lighting with shadows
	float3 diffuse_light = ubo.ambient_light_color.xyz; // already pre-multiplied by intensity on CPU
	float3 specular_light = float3(0.0, 0.0, 0.0);
	float3 view_dir = normalize(ubo.camera_position.xyz - in_vert.frag_pos_world);

	// Point lights
	for (uint32_t i = 0; i < ubo.num_lights; i++) {
		float3 light_dir = ubo.point_lights[i].position.xyz - in_vert.frag_pos_world;
		float3 L = normalize(light_dir);
		float NdotL = max(dot(normal, L), 0.0);

		if (NdotL <= 0.0)
			continue;

		// KHR_lights_punctual attenuation
		float distance_sq = dot(light_dir, light_dir);
		float range = ubo.point_lights[i].position.w;
		float attenuation;
		if (range > 0.0) {
			float dist = sqrt(distance_sq);
			float ratio = dist / range;
			float ratio2 = ratio * ratio;
			float ratio4 = ratio2 * ratio2;
			float window = clamp(1.0 - ratio4, 0.0, 1.0);
			attenuation = (window * window) / max(distance_sq, 0.0001);
		} else {
			attenuation = 1.0 / max(distance_sq, 0.0001);
		}
		float3 light_color = ubo.point_lights[i].color.xyz * attenuation;
		diffuse_light += light_color * NdotL * point_shadow_terms[i];

		float3 H = normalize(L + view_dir);
		float blinn = dot(normal, H);
		blinn = clamp(blinn, 0.0, 1.0);
		blinn = pow(blinn, 256.0);
		specular_light += light_color * blinn * point_shadow_terms[i];
	}

	// Directional lights (no distance attenuation)
	for (uint32_t i = 0; i < ubo.num_dir_lights; i++) {
		float3 L = normalize(-ubo.dir_lights[i].direction.xyz);
		float NdotL = max(dot(normal, L), 0.0);
		if (NdotL <= 0.0) continue;

		float3 light_color = ubo.dir_lights[i].color.xyz;
		diffuse_light += light_color * NdotL * dir_shadow_terms[i];

		float3 H = normalize(L + view_dir);
		float blinn = clamp(dot(normal, H), 0.0, 1.0);
		blinn = pow(blinn, 256.0);
		specular_light += light_color * blinn * dir_shadow_terms[i];
	}

	float3 final_color = base_color.rgb * diffuse_light + specular_light * base_color.rgb;
	return float4(final_color, base_color.a);
}
