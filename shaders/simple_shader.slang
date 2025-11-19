#define PI 3.1415926536

struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_color : COLOR;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
};

struct PointLight {
	float4 position;
	float4 color; // .w = intensity
	float4x4 light_view;          // light's view matrix (for shadow pass)
	float4x4 light_proj;          // light's projection matrix (for shadow pass)
	//float shadow_bias;
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
	float4 camera_position;
	float4 ambient_light_color;
	PointLight point_lights[20]; // MAX_LIGHTS TODO use specialization constant
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
	float3 frag_color;
	float2 frag_tex_coord;
	float4 shadow_coord[6];
};

static const float4x4 bias_matrix = float4x4(
    0.5, 0.0, 0.0, 0.5,
    0.0, 0.5, 0.0, 0.5,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0);

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
    VertexOutput output;

	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	float4 world_pos = mul(push_constants.transform, pos); // apply per-object transform

	output.pos = mul(ubo.proj, mul(ubo.view, world_pos)); // view and projection
	output.frag_pos_world = world_pos.xyz;

	// Transform normal and tangent to world space
	float3 normal;
	normal.x = dot(push_constants.normal_col0.xyz, input.in_normal);
	normal.y = dot(push_constants.normal_col1.xyz, input.in_normal);
	normal.z = dot(push_constants.normal_col2.xyz, input.in_normal);
	output.frag_normal_world = normalize(normal);
	// pass through
    output.frag_color = input.in_color;
	output.frag_tex_coord = input.in_tex_coord;

	 // calculate shadow coordinates only if shadows are enabled
	if (ubo.shadow_mode != 0) {
		for (uint32_t i = 0; i < ubo.num_lights; i++) {
			output.shadow_coord[i] = mul(bias_matrix, mul(ubo.point_lights[i].light_proj, mul(ubo.point_lights[i].light_view, mul(push_constants.transform, float4(input.in_pos, 1.0)))));
		}
	}
    return output;
}

// binding
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
	float scale = 1.5f;
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
	// Visualize shadow map of point light index 1 on textured quad
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

	float3 normal = normalize(in_vert.frag_normal_world);

	// visualize normal vector
	if (ubo.render_mode == RenderMode::NORMAL_VECTOR)
		return float4(0.5 * normal + 0.5, 1.0f);

	// Textured objects: use albedo texture with lighting
	float4 base_color = float4(in_vert.frag_color, 1.0f);
	if (push_constants.has_texture > 0.5f) {
		base_color = albedo_texture.Sample(in_vert.frag_tex_coord);
		if (base_color.a <= 0.001f)
			discard;
		return base_color;
	}

	// Simple lighting and shadow for non-textured objects
	float3 diffuse_light = ubo.ambient_light_color.xyz * ubo.ambient_light_color.w;
	for (uint32_t i = 0; i < ubo.num_lights; i++) {
		float3 light_dir = ubo.point_lights[i].position.xyz - in_vert.frag_pos_world;
		float3 L = normalize(light_dir);
		float NdotL = max(dot(normal, L), 0.0);

		if (NdotL <= 0.0)
			continue;

		// Calculate shadow term
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

		float attenuation = 1.0 / (0.01 * dot(light_dir, light_dir) + 0.1);
		float3 light_color = ubo.point_lights[i].color.xyz * ubo.point_lights[i].color.w * attenuation;
		diffuse_light += light_color * NdotL * shadow;
	}

	float3 final_color = base_color.rgb * diffuse_light;
	return float4(final_color, base_color.a);
}
