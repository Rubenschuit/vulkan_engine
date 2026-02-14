struct VertexInput {
	float3 in_pos : POSITION;
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
};
[vk::binding(0, 0)] // binding 0, set 0
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	float4x4 transform;
	float4 params;  // x=exposure, yzw=tint (RGB)
	float output_depth_one;  // 1.0 = output depth 1.0 for background pass (eEqual test)
};
[push_constant]
PushConstantData push_constants;

struct VertexOutput {
	float4 pos : SV_Position;
	float3 cube_dir;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
	VertexOutput output;

	// Convert Y-up to engine Z-up
	output.cube_dir = normalize(float3(input.in_pos.x, input.in_pos.z, input.in_pos.y));
	float4 pos = float4(input.in_pos, 1.0); // homogeneous coordinates
	float4 world_pos = mul(push_constants.transform, pos);
	output.pos = mul(ubo.proj, mul(ubo.view, world_pos)); // view and projection

	return output;
}

[vk::binding(0, 1)] // binding 0, set 1
SamplerCube cube_texture;

struct FragmentOutput {
	float4 color : SV_Target;
	float depth : SV_Depth;
};

[shader("fragment")]
FragmentOutput fragMain(VertexOutput in_vert, uint id : SV_SampleIndex) {
	float4 sampled = cube_texture.Sample(in_vert.cube_dir);
	float exposure = push_constants.params.x;
	float3 tint = push_constants.params.yzw;
	FragmentOutput o;
	o.color = float4(sampled.rgb * exposure * tint, sampled.a);
	// When output_depth_one > 0 (background pass), output 1.0 so depth test eEqual passes only where buffer is clear.
	o.depth = (push_constants.output_depth_one > 0.0) ? 1.0 : in_vert.pos.z;
	return o;
}
