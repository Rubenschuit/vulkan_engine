#include "ve_common.slangh"

struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
	float4 in_tangent : TANGENT; // xyz = tangent, w = bitangent sign
};

// Bindless textures (set 1): sampler at binding 0, texture array at binding 1
[vk::binding(0, 1)]
SamplerState bindless_sampler;
[vk::binding(1, 1)]
Texture2D bindless_textures[];

struct VertexOutput {
	float4 pos : SV_Position;
    float3 frag_pos_world;
	float3 frag_normal_world;
	float3 frag_tangent_world;
	float3 frag_bitangent_world;
	float2 frag_tex_coord;
	float4 shadow_coord[MAX_SHADOW_LIGHTS];
	nointerpolation uint material_index;
	nointerpolation uint lod_level;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input, uint instance_id : SV_VulkanInstanceID) {
    VertexOutput output;

	InstanceData inst = instances[instance_id];
	output.material_index = inst.material_index;
	output.lod_level = inst.lod_level;

	float4 pos = float4(input.in_pos, 1.0);
	float4 world_pos = mul(inst.transform, pos);

	output.pos = mul(ubo.projection_view, world_pos);
	output.pos.z -= inst.depth_offset * output.pos.w;
	output.frag_pos_world = world_pos.xyz;

	// Transform normal and tangent to world space
	float3x3 normal_matrix = float3x3(
		float3(inst.normal_col0.x, inst.normal_col1.x, inst.normal_col2.x),
		float3(inst.normal_col0.y, inst.normal_col1.y, inst.normal_col2.y),
		float3(inst.normal_col0.z, inst.normal_col1.z, inst.normal_col2.z)
	);
	float3 tangent = mul(normal_matrix, input.in_tangent.xyz);
	float3 normal = normalize(mul(normal_matrix, input.in_normal));
	float3 bitangent = normalize(cross(normal, tangent) * input.in_tangent.w);

	output.frag_normal_world = normal;
	output.frag_tangent_world = tangent;
	output.frag_bitangent_world = bitangent;
	float2 tex_coord = input.in_tex_coord;
	if ((inst.material_flags & 8u) != 0u)  // flip_tex_coord_v (bit 3)
		tex_coord.y = 1.0f - tex_coord.y;
	output.frag_tex_coord = tex_coord;

	if (SHADOW_MODE_SPEC != ShadowMode::DISABLED) {
		for (uint32_t i = 0; i < ubo.num_shadow_lights; i++)
			output.shadow_coord[i] = mul(ubo.shadow_lights[i].shadow_matrix, world_pos);
	}

    return output;
}

// ----------------------------------------------------------------------------
// PBR Helper Functions
// ----------------------------------------------------------------------------

float3 fresnelSchlick(float cos_theta, float3 F0) {
	float t = clamp(1.0 - cos_theta, 0.0, 1.0);
	float t2 = t * t;
	return F0 + (1.0 - F0) * (t2 * t2 * t);
}

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
	MaterialGPU mat = materials[in_vert.material_index];

	float2 tex_coord = in_vert.frag_tex_coord;
	float4 albedo = bindless_textures[NonUniformResourceIndex(mat.albedo_index)]
		.Sample(bindless_sampler, tex_coord);

	// glTF alphaMode: OPAQUE(0)=force alpha 1, MASK(1)=discard below cutoff, BLEND(2)=use texture alpha
	uint32_t alpha_mode = mat.material_flags & 3u;
	if (alpha_mode == 0)
		albedo.a = 1.0;
	else if (alpha_mode == 1 && albedo.a < mat.alpha_cutoff)
		discard;

	float4 mr = bindless_textures[NonUniformResourceIndex(mat.metallic_roughness_index)]
		.Sample(bindless_sampler, tex_coord);
	float4 normal_map = bindless_textures[NonUniformResourceIndex(mat.normal_index)]
		.Sample(bindless_sampler, tex_coord);
	float4 occlusion_sample = bindless_textures[NonUniformResourceIndex(mat.occlusion_index)]
		.Sample(bindless_sampler, tex_coord);
	float3 emissive_sample = bindless_textures[NonUniformResourceIndex(mat.emissive_index)]
		.Sample(bindless_sampler, tex_coord).rgb;

	albedo *= mat.base_color_factor;
	float metallic;
	float roughness;
	float3 F0;
	if ((mat.material_flags & 16u) != 0u) {
		// Spec-gloss texture: params.y is glossinessFactor, mr.a is per-texel glossiness
		metallic = 0.0;
		roughness = 1.0 - mr.a * mat.params.y;
		F0 = mat.extra.xyz * mr.rgb;
	} else {
		// Metallic-roughness texture
		metallic = mr.b * mat.params.x;
		roughness = mr.g * mat.params.y;
		F0 = lerp(mat.extra.xyz, albedo.rgb, metallic);
	}
	roughness = max(roughness, 0.045);
	float transmission = mat.params.w;
	emissive_sample = emissive_sample * mat.emissive_pad.xyz * mat.params.z;

	float3 N = normalize(in_vert.frag_normal_world);
	float3 T = normalize(in_vert.frag_tangent_world);
	float3 B = normalize(in_vert.frag_bitangent_world);
	float3 world_normal = N;
	if (ubo.render_mode == RenderMode::BRDF_MICROFACET || ubo.render_mode == RenderMode::NORMAL_MAP) {
		float3 tangent_space_normal = normalize(normal_map.xyz * 2.0 - float3(1.0, 1.0, 1.0));
		world_normal = normalize(tangent_space_normal.x * T + tangent_space_normal.y * B + tangent_space_normal.z * N);
	}

	if (ubo.render_mode == RenderMode::NORMAL_VECTOR)
		return float4(0.5 * N + 0.5, 1.0f);
	else if (ubo.render_mode == RenderMode::TANGENT_VECTOR)
		return float4(0.5 * T + 0.5, 1.0f);
	else if (ubo.render_mode == RenderMode::BITANGENT_VECTOR)
		return float4(0.5 * B + 0.5, 1.0f);
	else if (ubo.render_mode == RenderMode::NORMAL_MAP)
		return float4(0.5 * world_normal + 0.5, 1.0f);
	else if (ubo.render_mode == RenderMode::LOD_LEVEL)
		return float4(lodDebugColor(albedo.rgb, in_vert.lod_level), albedo.a);

	float3 view_dir = normalize(ubo.camera_position.xyz - in_vert.frag_pos_world);
	float frag_view_z = -(mul(ubo.view, float4(in_vert.frag_pos_world, 1.0))).z;

	// CSM shadow for directional light
	float csm_shadow = 1.0;
	uint csm_cascade_idx = 0;
	float csm_blend_t = 0.0;
	if (SHADOW_MODE_SPEC != ShadowMode::DISABLED && ubo.csm_cascade_count > 0) {
		if (SHADOW_MASK_SPEC != 0) {
			float4 prev_clip = mul(ubo.prev_projection_view, float4(in_vert.frag_pos_world, 1.0));
			float2 prev_ndc = prev_clip.xy / prev_clip.w;
			float2 prev_uv = prev_ndc * 0.5 + 0.5;
			csm_shadow = shadow_mask.SampleLevel(shadow_mask_sampler, prev_uv, 0);
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

	// Cluster heatmap debug
	if (ubo.render_mode == RenderMode::CLUSTER_HEATMAP && cluster_params.cluster_enabled != 0) {
		uint cid = getClusterIndex(in_vert.pos.xy, frag_view_z, cluster_params);
		uint count = cluster_light_counts[cid].y;
		float3 heat = clusterHeatmapColor(in_vert.pos.xy, frag_view_z, count, cluster_params);
		return float4(heat * albedo.rgb, albedo.a);
	}

	// Precompute light-invariant terms
	float NdotV = clamp(dot(world_normal, view_dir), 0.0, 1.0);
	float G1_V = geometrySchlickGGX(NdotV, roughness);
	float kD_factor = (1.0 - metallic) * (1.0 - transmission);
	float3 diffuse_brdf = albedo.rgb / PI;

	// Accumulate point + spot light contribution
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

			float distance_sq = dot(light_dir, light_dir);
			float attenuation = calculateAttenuation(distance_sq, light.position.w);

			if (light_idx >= cluster_params.num_point_lights) {
				uint spot_idx = light_idx - cluster_params.num_point_lights;
				float cos_outer = ubo.spot_lights[spot_idx].direction.w;
				float cos_inner = ubo.spot_lights[spot_idx].color.w;
				float spot_atten = calculateSpotAttenuation(L,
					ubo.spot_lights[spot_idx].direction.xyz, cos_outer, cos_inner);
				if (spot_atten <= 0.0) continue;
				attenuation *= spot_atten;
			}

			float3 H = normalize(view_dir + L);
			float3 radiance = light.color.xyz * attenuation;

			float3 F = fresnelSchlick(max(dot(H, view_dir), 0.0), F0);
			float D = distributionGGX(world_normal, H, roughness);
			float G = G1_V * geometrySchlickGGX(NdotL, roughness);
			float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.00001);

			float3 kD = (1.0 - F) * kD_factor;
			uint shadow_key = light_idx;
			if (light_idx >= cluster_params.num_point_lights)
				shadow_key = MAX_LIGHTS + (light_idx - cluster_params.num_point_lights);
			float pt_shadow = getPointShadow(shadow_key, shadow_terms, shadow_for_light);
			Lo += (kD * diffuse_brdf + specular) * radiance * NdotL * pt_shadow;
		}
	} else {
		// Brute-force point lights
		for (uint32_t i = 0; i < ubo.num_lights; i++) {
			float3 light_dir = ubo.point_lights[i].position.xyz - in_vert.frag_pos_world;
			float3 L = normalize(light_dir);
			float NdotL = clamp(dot(world_normal, L), 0.0, 1.0);
			if (NdotL <= 0.0) continue;

			float3 H = normalize(view_dir + L);
			float distance_sq = dot(light_dir, light_dir);
			float attenuation = calculateAttenuation(distance_sq, ubo.point_lights[i].position.w);
			float3 radiance = ubo.point_lights[i].color.xyz * attenuation;

			float3 F = fresnelSchlick(max(dot(H, view_dir), 0.0), F0);
			float D = distributionGGX(world_normal, H, roughness);
			float G = G1_V * geometrySchlickGGX(NdotL, roughness);
			float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.00001);

			float3 kD = (1.0 - F) * kD_factor;
			float pt_shadow = getPointShadow(i, shadow_terms, shadow_for_light);
			Lo += (kD * diffuse_brdf + specular) * radiance * NdotL * pt_shadow;
		}

		// Brute-force spot lights
		for (uint32_t i = 0; i < ubo.num_spot_lights; i++) {
			float3 light_dir = ubo.spot_lights[i].position.xyz - in_vert.frag_pos_world;
			float3 L = normalize(light_dir);
			float NdotL = clamp(dot(world_normal, L), 0.0, 1.0);
			if (NdotL <= 0.0) continue;

			float distance_sq = dot(light_dir, light_dir);
			float attenuation = calculateAttenuation(distance_sq, ubo.spot_lights[i].position.w);

			float cos_outer = ubo.spot_lights[i].direction.w;
			float cos_inner = ubo.spot_lights[i].color.w;
			float spot_atten = calculateSpotAttenuation(L,
				ubo.spot_lights[i].direction.xyz, cos_outer, cos_inner);
			if (spot_atten <= 0.0) continue;
			attenuation *= spot_atten;

			float3 H = normalize(view_dir + L);
			float3 radiance = ubo.spot_lights[i].color.xyz * attenuation;

			float3 F = fresnelSchlick(max(dot(H, view_dir), 0.0), F0);
			float D = distributionGGX(world_normal, H, roughness);
			float G = G1_V * geometrySchlickGGX(NdotL, roughness);
			float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.00001);

			float3 kD = (1.0 - F) * kD_factor;
			float spot_shadow = getPointShadow(MAX_LIGHTS + i, shadow_terms, shadow_for_light);
			Lo += (kD * diffuse_brdf + specular) * radiance * NdotL * spot_shadow;
		}
	}

	// Directional lights
	for (uint32_t i = 0; i < ubo.num_dir_lights; i++) {
		float3 L = normalize(-ubo.dir_lights[i].direction.xyz);
		float NdotL = clamp(dot(world_normal, L), 0.0, 1.0);
		if (NdotL <= 0.0) continue;

		float3 H = normalize(view_dir + L);
		float3 radiance = ubo.dir_lights[i].color.xyz;

		float3 F = fresnelSchlick(max(dot(H, view_dir), 0.0), F0);
		float D = distributionGGX(world_normal, H, roughness);
		float G = G1_V * geometrySchlickGGX(NdotL, roughness);
		float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.00001);
		float3 kD = (1.0 - F) * kD_factor;

		float dir_shadow = (i == ubo.csm_dir_light_index) ? csm_shadow : 1.0;
		Lo += (kD * diffuse_brdf + specular) * radiance * NdotL * dir_shadow;
	}

	// SSAO
	float2 screen_uv = in_vert.pos.xy / ubo.screen_size;
	float ssao = ao_texture.SampleLevel(ao_sampler, screen_uv, 0);

	// Ambient
	float3 ambient = ubo.ambient_light_color.xyz * albedo.rgb * occlusion_sample.r * ssao;
	ambient *= 1.0 - transmission;

	c.rgb = Lo + ambient + emissive_sample;
	static const float HIGH_TRANSMISSION_THRESHOLD = 0.5;
	float raw_alpha = (1.0 - transmission) * albedo.a;
	c.a = (transmission > HIGH_TRANSMISSION_THRESHOLD) ? max(raw_alpha, 0.45) : raw_alpha;

	return c;
}
