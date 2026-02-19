#include "ve_common.slangh"

struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
};

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
	float4 shadow_coord[MAX_SHADOW_LIGHTS];
};

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

[shader("fragment")]

float4 fragMain(VertexOutput in_vert) : SV_Target {

	/*
	// DEBUG: Visualize shadow map of point light index 1 on textured quad
	if (push_constants.has_texture > 0.5f && ubo.num_lights > 0 && SHADOW_MODE_SPEC != ShadowMode::DISABLED) {
		float shadow_depth = shadow_mask.SampleLevel(shadow_mask_sampler, in_vert.frag_tex_coord, 0.0).r;

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
		base_color = albedo_texture.Sample(uv);
		if (base_color.a <= 0.001f)
			discard;
		return base_color;
	}

	// View-space depth (used by CSM and cluster lookup)
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
		} else {
			csm_shadow = calculateCSMShadow(in_vert.frag_pos_world, frag_view_z, csm_cascade_idx, csm_blend_t);
		}
	}

	if (ubo.render_mode == RenderMode::CSM_CASCADE) {
		if (SHADOW_MASK_SPEC != 0)
			return float4(csm_shadow, csm_shadow, csm_shadow, 1.0);
		return float4(csmDebugColor(base_color.rgb, csm_cascade_idx, csm_blend_t, csm_shadow), base_color.a);
	}

	// Point light shadow terms
	float shadow_terms[MAX_SHADOW_LIGHTS];
	uint32_t shadow_for_light[MAX_SHADOW_LIGHTS];
	calculatePointShadowTerms(in_vert.shadow_coord, shadow_terms, shadow_for_light);

	// Cluster heatmap debug
	if (ubo.render_mode == RenderMode::CLUSTER_HEATMAP && cluster_params.cluster_enabled != 0) {
		uint cid = getClusterIndex(in_vert.pos.xy, frag_view_z, cluster_params);
		uint count = cluster_light_counts[cid].y;
		float3 heat = clusterHeatmapColor(in_vert.pos.xy, frag_view_z, count, cluster_params);
		return float4(heat * base_color.rgb, base_color.a);
	}

	// Apply lighting with shadows
	float3 diffuse_light = ubo.ambient_light_color.xyz;
	float3 specular_light = float3(0.0, 0.0, 0.0);
	float3 view_dir = normalize(ubo.camera_position.xyz - in_vert.frag_pos_world);

	// Point lights
	if (cluster_params.cluster_enabled != 0) {
		uint cid = getClusterIndex(in_vert.pos.xy, frag_view_z, cluster_params);
		uint2 counts = cluster_light_counts[cid];
		for (uint j = 0; j < counts.y; j++) {
			uint light_idx = cluster_light_indices[counts.x + j];
			PointLight light = cluster_lights[light_idx];

			float3 light_dir = light.position.xyz - in_vert.frag_pos_world;
			float3 L = normalize(light_dir);
			float NdotL = max(dot(normal, L), 0.0);
			if (NdotL <= 0.0) continue;

			float distance_sq = dot(light_dir, light_dir);
			float attenuation = calculateAttenuation(distance_sq, light.position.w);
			float3 light_color = light.color.xyz * attenuation;

			float pt_shadow = getPointShadow(light_idx, shadow_terms, shadow_for_light);
			diffuse_light += light_color * NdotL * pt_shadow;

			float3 H = normalize(L + view_dir);
			float blinn = clamp(dot(normal, H), 0.0, 1.0);
			blinn = pow(blinn, 256.0);
			specular_light += light_color * blinn * pt_shadow;
		}
	} else {
		for (uint32_t i = 0; i < ubo.num_lights; i++) {
			float3 light_dir = ubo.point_lights[i].position.xyz - in_vert.frag_pos_world;
			float3 L = normalize(light_dir);
			float NdotL = max(dot(normal, L), 0.0);
			if (NdotL <= 0.0) continue;

			float distance_sq = dot(light_dir, light_dir);
			float attenuation = calculateAttenuation(distance_sq, ubo.point_lights[i].position.w);
			float3 light_color = ubo.point_lights[i].color.xyz * attenuation;

			float pt_shadow = getPointShadow(i, shadow_terms, shadow_for_light);
			diffuse_light += light_color * NdotL * pt_shadow;

			float3 H = normalize(L + view_dir);
			float blinn = clamp(dot(normal, H), 0.0, 1.0);
			blinn = pow(blinn, 256.0);
			specular_light += light_color * blinn * pt_shadow;
		}
	}

	// Directional lights (no distance attenuation)
	for (uint32_t i = 0; i < ubo.num_dir_lights; i++) {
		float3 L = normalize(-ubo.dir_lights[i].direction.xyz);
		float NdotL = max(dot(normal, L), 0.0);
		if (NdotL <= 0.0) continue;

		float dir_shadow = (i == ubo.csm_dir_light_index) ? csm_shadow : 1.0;
		float3 light_color = ubo.dir_lights[i].color.xyz;
		diffuse_light += light_color * NdotL * dir_shadow;

		float3 H = normalize(L + view_dir);
		float blinn = clamp(dot(normal, H), 0.0, 1.0);
		blinn = pow(blinn, 256.0);
		specular_light += light_color * blinn * dir_shadow;
	}

	float3 final_color = base_color.rgb * diffuse_light + specular_light * base_color.rgb;
	return float4(final_color, base_color.a);
}
