#include "ve_common.slangh"

struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
	float4 in_tangent : TANGENT; // xyz = tangent, w = bitangent sign
};

struct PushConstantData {
	[vk::offset(0)]  float    has_texture;
	[vk::offset(4)]  float    alpha_cutoff;
	[vk::offset(8)]  uint32_t material_flags;  // bits 0-1: alpha_mode, bit 2: double_sided, bit 3: flip_tex_coord_v, bit 4: use_spec_gloss_texture
	[vk::offset(12)] uint32_t instance_offset;  // SSBO offset (SV_InstanceID is 0-based in Slang)
	[vk::offset(16)] float    depth_offset;     // clip-space Z offset (>0 pushes towards camera)
	[vk::offset(20)] uint32_t lod_level;        // LOD level for debug visualization
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
	float4 pos : SV_Position;
    float3 frag_pos_world;
	float3 frag_normal_world;
	float3 frag_tangent_world;
	float3 frag_bitangent_world;
	float2 frag_tex_coord;
	float4 shadow_coord[MAX_SHADOW_LIGHTS];
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input, uint instance_id : SV_InstanceID) {
    VertexOutput output;

	InstanceData inst = instances[instance_id + push_constants.instance_offset];

	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	float4 world_pos = mul(inst.transform, pos); // apply per-instance transform

	output.pos = mul(ubo.projection_view, world_pos);
	// Push geometry towards camera in clip space (for decal overlay z-fighting).
	// Multiplying by w makes the offset proportional to depth, so it's consistent at all distances.
	output.pos.z -= push_constants.depth_offset * output.pos.w;
	output.frag_pos_world = world_pos.xyz;

	// Transform normal and tangent to world space
	float3x3 normal_matrix = float3x3(
		float3(inst.normal_col0.x, inst.normal_col1.x, inst.normal_col2.x),
		float3(inst.normal_col0.y, inst.normal_col1.y, inst.normal_col2.y),
		float3(inst.normal_col0.z, inst.normal_col1.z, inst.normal_col2.z)
	);
	float3 tangent = mul(normal_matrix, input.in_tangent.xyz);
	float3 normal = normalize(mul(normal_matrix, input.in_normal));

	// Compute bitangent as cross product of normal and tangent with proper handedness ( *-1 or *1 )
	float3 bitangent = normalize(cross(normal, tangent) * input.in_tangent.w);

	output.frag_normal_world = normal;
	output.frag_tangent_world = tangent;
	output.frag_bitangent_world = bitangent;
	float2 tex_coord = input.in_tex_coord;
	if ((push_constants.material_flags & 8u) != 0u)  // flip_tex_coord_v (bit 3)
		tex_coord.y = 1.0f - tex_coord.y;
	output.frag_tex_coord = tex_coord;

    // calculate shadow coordinates only if shadows are enabled
	if (SHADOW_MODE_SPEC != ShadowMode::DISABLED) {
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

	// glTF alphaMode: OPAQUE(0)=force alpha 1, MASK(1)=discard below cutoff, BLEND(2)=use texture alpha
	uint32_t alpha_mode = push_constants.material_flags & 3u;
	if (alpha_mode == 0)
		albedo.a = 1.0;
	else if (alpha_mode == 1 && albedo.a < push_constants.alpha_cutoff)
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
	else if (ubo.render_mode == RenderMode::LOD_LEVEL)
		return float4(lodDebugColor(albedo.rgb, push_constants.lod_level), albedo.a);

	// View direction and view-space depth (used by CSM and cluster lookup)
	float3 view_dir = normalize(ubo.camera_position.xyz - in_vert.frag_pos_world);
	float frag_view_z = -(mul(ubo.view, float4(in_vert.frag_pos_world, 1.0))).z;

	// CSM shadow for directional light (first dir light with CSM)
	float csm_shadow = 1.0;
	uint csm_cascade_idx = 0;
	float csm_blend_t = 0.0;
	if (SHADOW_MODE_SPEC != ShadowMode::DISABLED && ubo.csm_cascade_count > 0) {
		if (SHADOW_MASK_SPEC != 0) {
			float4 prev_clip = mul(ubo.prev_projection_view, float4(in_vert.frag_pos_world, 1.0));
			float2 prev_ndc = prev_clip.xy / prev_clip.w;
			float2 prev_uv = prev_ndc * 0.5 + 0.5;
			csm_shadow = shadow_mask.SampleLevel(shadow_mask_sampler, prev_uv, 0);
			// Compute cascade index for debug visualization
			if (ubo.render_mode == RenderMode::CSM_CASCADE)
				calculateCSMShadow(in_vert.frag_pos_world, frag_view_z, world_normal, csm_cascade_idx, csm_blend_t);
		} else {
			csm_shadow = calculateCSMShadow(in_vert.frag_pos_world, frag_view_z, world_normal, csm_cascade_idx, csm_blend_t);
		}
	}

	if (ubo.render_mode == RenderMode::CSM_CASCADE)
		return float4(csmDebugColor(albedo.rgb, csm_cascade_idx, csm_blend_t, csm_shadow), albedo.a);

	// Point light shadow terms
	float shadow_terms[MAX_SHADOW_LIGHTS];
	uint32_t shadow_for_light[MAX_SHADOW_LIGHTS];
	calculatePointShadowTerms(in_vert.shadow_coord, shadow_terms, shadow_for_light);

	// Cluster heatmap debug: visualize lights-per-cluster as a heat gradient
	if (ubo.render_mode == RenderMode::CLUSTER_HEATMAP && cluster_params.cluster_enabled != 0) {
		uint cid = getClusterIndex(in_vert.pos.xy, frag_view_z, cluster_params);
		uint count = cluster_light_counts[cid].y;
		float3 heat = clusterHeatmapColor(in_vert.pos.xy, frag_view_z, count, cluster_params);
		return float4(heat * albedo.rgb, albedo.a);
	}

	// Accumulate point light contribution
	float3 Lo = float3(0.0);
	if (cluster_params.cluster_enabled != 0) {
		uint cid = getClusterIndex(in_vert.pos.xy, frag_view_z, cluster_params);
		uint2 counts = cluster_light_counts[cid];
		for (uint j = 0; j < counts.y; j++) {
			uint light_idx = cluster_light_indices[counts.x + j];
			PointLight light = cluster_lights[light_idx];

			float3 light_dir = light.position.xyz - in_vert.frag_pos_world;
			float3 L = normalize(light_dir);
			float NdotL = clamp(dot(world_normal, L), 0.0, 1.0);
			if (NdotL <= 0.0) continue;

			float NdotV = clamp(dot(world_normal, view_dir), 0.0, 1.0);
			float3 H = normalize(view_dir + L);
			float distance_sq = dot(light_dir, light_dir);
			float attenuation = calculateAttenuation(distance_sq, light.position.w);
			float3 radiance = light.color.xyz * attenuation;

			float3 F = fresnelSchlick(max(dot(H, view_dir), 0.0), F0);
			float D = distributionGGX(world_normal, H, roughness);
			float G = geometrySmith(NdotV, NdotL, roughness);
			float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.00001);

			float3 kD = (1.0 - F) * (1.0 - metallic) * (1.0 - transmission);
			float pt_shadow = getPointShadow(light_idx, shadow_terms, shadow_for_light);
			Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL * pt_shadow;
		}
	} else {
		for (uint32_t i = 0; i < ubo.num_lights; i++) {
			float3 light_dir = ubo.point_lights[i].position.xyz - in_vert.frag_pos_world;
			float3 L = normalize(light_dir);
			float NdotL = clamp(dot(world_normal, L), 0.0, 1.0);
			if (NdotL <= 0.0) continue;

			float NdotV = clamp(dot(world_normal, view_dir), 0.0, 1.0);
			float3 H = normalize(view_dir + L);
			float distance_sq = dot(light_dir, light_dir);
			float attenuation = calculateAttenuation(distance_sq, ubo.point_lights[i].position.w);
			float3 radiance = ubo.point_lights[i].color.xyz * attenuation;

			float3 F = fresnelSchlick(max(dot(H, view_dir), 0.0), F0);
			float D = distributionGGX(world_normal, H, roughness);
			float G = geometrySmith(NdotV, NdotL, roughness);
			float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.00001);

			float3 kD = (1.0 - F) * (1.0 - metallic) * (1.0 - transmission);
			float pt_shadow = getPointShadow(i, shadow_terms, shadow_for_light);
			Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL * pt_shadow;
		}
	}

	// Directional lights (no distance attenuation)
	for (uint32_t i = 0; i < ubo.num_dir_lights; i++) {
		float3 L = normalize(-ubo.dir_lights[i].direction.xyz);
		float NdotL = clamp(dot(world_normal, L), 0.0, 1.0);
		if (NdotL <= 0.0) continue;

		float NdotV = clamp(dot(world_normal, view_dir), 0.0, 1.0);
		float3 H = normalize(view_dir + L);
		float3 radiance = ubo.dir_lights[i].color.xyz; // no attenuation

		float3 F = fresnelSchlick(max(dot(H, view_dir), 0.0), F0);
		float D = distributionGGX(world_normal, H, roughness);
		float G = geometrySmith(NdotV, NdotL, roughness);
		float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.00001);
		float3 kD = (1.0 - F) * (1.0 - metallic) * (1.0 - transmission);

		float dir_shadow = (i == ubo.csm_dir_light_index) ? csm_shadow : 1.0;
		Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL * dir_shadow;
	}

	// Sample screen-space ambient occlusion (GTAO)
	float2 screen_uv = in_vert.pos.xy / ubo.screen_size;
	float ssao = ao_texture.SampleLevel(ao_sampler, screen_uv, 0);

	// Ambient lighting (scaled by material occlusion, SSAO, and transmission)
	float3 ambient = ubo.ambient_light_color.xyz * albedo.rgb * occlusion_sample.r * ssao;
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
