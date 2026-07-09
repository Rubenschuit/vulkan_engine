// Depth prepass with a G-buffer color output: world-space geometric normal (xyz)
// + material roughness (w, factor x metallic-roughness texture).
// Alpha-masked geometry is excluded, so masked pixels have
// no depth or normal here.

#include "ve_common.slangh"

[vk::binding(0, 1)]
SamplerState bindless_sampler;
[vk::binding(1, 1)]
Texture2D bindless_textures[];

struct VertexInput {
	float3 in_pos : POSITION;
	float3 in_normal : NORMAL;
	float2 in_tex_coord : TEXCOORD0;
};

struct VertexOutput {
	float4 pos : SV_Position;
	float3 world_normal : NORMAL;
	float3 frag_pos_world : WORLD_POS;
	float2 tex_coord : TEXCOORD0;
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

	float3x3 normal_matrix = float3x3(
		float3(inst.normal_col0.x, inst.normal_col1.x, inst.normal_col2.x),
		float3(inst.normal_col0.y, inst.normal_col1.y, inst.normal_col2.y),
		float3(inst.normal_col0.z, inst.normal_col1.z, inst.normal_col2.z)
	);
	output.world_normal = mul(normal_matrix, input.in_normal);

	float2 tex_coord = input.in_tex_coord;
	if ((inst.material_flags & MAT_FLAG_FLIP_TEX_V) != 0u)
		tex_coord.y = 1.0 - tex_coord.y;
	output.tex_coord = tex_coord;
	output.material_index = inst.material_index;

	return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target0 {
	MaterialGPU mat = materials[in_vert.material_index];

	float3 normal = normalize(in_vert.world_normal);
	// Flip double-sided normals toward the camera by view vector
	float3 view_dir = normalize(ubo.camera_position.xyz - in_vert.frag_pos_world);
	if ((mat.material_flags & MAT_FLAG_DOUBLE_SIDED) != 0u && dot(normal, view_dir) < 0.0)
		normal = -normal;

	// Handle mr texture / spec-gloss texture / default roughness factor
	float2 uv_mr = applyUvTransform(in_vert.tex_coord, mat.uv_metal_rough, mat.uv_rot_a.y);
	float4 mr_sample = bindless_textures[NonUniformResourceIndex(mat.metallic_roughness_index)]
		.Sample(bindless_sampler, uv_mr);
	float roughness = (mat.material_flags & MAT_FLAG_SPEC_GLOSS) != 0u
		? 1.0 - mr_sample.a * mat.pbr_params.y
		: mr_sample.g * mat.pbr_params.y;

	return float4(normal, roughness);
}
