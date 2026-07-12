// Geometry prepass with a G-buffer color output: world-space normal-mapped
// normal (xyz) + material roughness (w, factor x metallic-roughness texture).
// ALPHA_MASK_SPEC=1 compiles the alpha-mask variant

#include "ve_common.slangh"

[vk::constant_id(6)] const uint ALPHA_MASK_SPEC = 0;

[vk::binding(0, 1)]
SamplerState bindless_sampler;
[vk::binding(1, 1)]
Texture2D bindless_textures[];

struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
	float4 in_tangent : TANGENT; // xyz = tangent, w = bitangent sign
	float4 in_color : COLOR0;
};

struct VertexOutput {
	float4 pos : SV_Position;
	float3 world_normal : NORMAL;
	float3 world_tangent : TANGENT;
	float3 world_bitangent : BITANGENT;
	float3 frag_pos_world : WORLD_POS;
	float2 tex_coord : TEXCOORD0;
	float color_a : COLOR_ALPHA; // COLOR_0 alpha participates in the mask cutoff
	nointerpolation uint material_index : MATERIAL_INDEX;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input, uint instance_id : SV_VulkanInstanceID) {
	VertexOutput output;

	// Meshlet path packs (gpu_id | meshlet_local << 16) into firstInstance.
	uint gpu_id = instance_id & 0xFFFFu;
	InstanceData inst = instances[gpu_id];

	float4 world_pos = mul(inst.transform, float4(input.in_pos, 1.0));
	output.pos = mul(ubo.projection_view, world_pos);
	output.frag_pos_world = world_pos.xyz;

	float3 normal, tangent, bitangent;
	makeWorldTbn(inst, input.in_normal, input.in_tangent, normal, tangent, bitangent);
	output.world_normal = normal;
	output.world_tangent = tangent;
	output.world_bitangent = bitangent;

	float2 tex_coord = input.in_tex_coord;
	if ((inst.material_flags & MAT_FLAG_FLIP_TEX_V) != 0u)
		tex_coord.y = 1.0 - tex_coord.y;
	output.tex_coord = tex_coord;
	output.color_a = input.in_color.a;
	output.material_index = inst.material_index;

	return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target0 {
	MaterialGPU mat = materials[in_vert.material_index];

	if (ALPHA_MASK_SPEC != 0u) {
		float2 uv_albedo = applyUvTransform(in_vert.tex_coord, mat.uv_albedo, mat.uv_rot_a.x);
		float tex_alpha = bindless_textures[NonUniformResourceIndex(mat.albedo_index)]
			.Sample(bindless_sampler, uv_albedo).a;
		if (alphaMaskCutoff(mat, tex_alpha, in_vert.color_a))
			discard;
	}

	float3 N = normalize(in_vert.world_normal);
	float3 T = normalize(in_vert.world_tangent);
	float3 B = normalize(in_vert.world_bitangent);

	float2 uv_normal = applyUvTransform(in_vert.tex_coord, mat.uv_normal, mat.uv_rot_a.z);
	float3 normal_map = bindless_textures[NonUniformResourceIndex(mat.normal_index)]
		.Sample(bindless_sampler, uv_normal).xyz;
	float3 normal = applyNormalMap(normal_map, T, B, N);
	float3 view_dir = normalize(ubo.camera_position.xyz - in_vert.frag_pos_world);
	normal = flipDoubleSided(normal, view_dir, mat.material_flags);

	// Handle mr texture / spec-gloss texture / default roughness factor
	float2 uv_mr = applyUvTransform(in_vert.tex_coord, mat.uv_metal_rough, mat.uv_rot_a.y);
	float4 mr_sample = bindless_textures[NonUniformResourceIndex(mat.metallic_roughness_index)]
		.Sample(bindless_sampler, uv_mr);
	float roughness = (mat.material_flags & MAT_FLAG_SPEC_GLOSS) != 0u
		? 1.0 - mr_sample.a * mat.pbr_params.y
		: mr_sample.g * mat.pbr_params.y;

	return float4(normal, roughness);
}
