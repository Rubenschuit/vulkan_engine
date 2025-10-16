// Render a simple camera-facing quad for each point light in the scene
static const float2 vertices[6] = float2[](
	float2(-0.5f, -0.5f),
	float2( 0.5f, -0.5f),
	float2( 0.5f,  0.5f),
	float2( 0.5f,  0.5f),
	float2(-0.5f,  0.5f),
	float2(-0.5f, -0.5f)
);

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	float4 position;
	float4 color; // .w = intensity
	float scale; // in camera space units
};
[push_constant]
PushConstantData push;

struct VertexOutput {
    float4 pos: SV_Position;
	float2 offset;
};

[shader("vertex")]
VertexOutput vertMain(uint id : SV_VertexID) {
    VertexOutput out;

	out.offset = vertices[id];

	float4 position_cameraspace = mul(ubo.view, push.position);
	position_cameraspace += float4(out.offset * push.scale, 0.0f, 0.0f);

 	out.pos = mul(ubo.proj, position_cameraspace);


    return out;
}

[shader("fragment")]
float4 fragMain(VertexOutput in) : SV_Target {
	float distance_squared = dot(in.offset, in.offset);
	float4 color = float4(push.color.rgb, 1.0f);
	if (distance_squared >= 0.25f) {
		discard;
	}
	else if (distance_squared >= 0.05f) {
		// fade out at the edges
		float alpha = (0.25f - distance_squared) / (0.25f - 0.05f);
		color.rgba *= sqrt(alpha); // smooth fade

	}
    return color;
}
