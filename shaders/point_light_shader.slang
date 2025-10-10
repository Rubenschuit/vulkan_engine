static const float2 vertices[6] = float2[](
	float2(-0.5f, -0.5f),
	float2( 0.5f, -0.5f),
	float2( 0.5f,  0.5f),
	float2( 0.5f,  0.5f),
	float2(-0.5f,  0.5f),
	float2(-0.5f, -0.5f)
);

struct PointLight {
	float4 position;
	float4 color;
};

struct UniformBuffer {
    float4x4 view;
    float4x4 proj;
	float4 ambient_light_color;
	PointLight point_lights[100]; // MAX_LIGHTS TODO use specialization constant
	uint32_t num_lights;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> ubo;

struct PushConstantData {
	float4 position;
	float4 color;
	float radius; // in camera space units
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
	position_cameraspace += float4(out.offset * push.radius, 0.0f, 0.0f);

 	out.pos = mul(ubo.proj, position_cameraspace);


    return out;
}

[shader("fragment")]
float4 fragMain(VertexOutput in) : SV_Target {
	if (dot(in.offset, in.offset) >= 0.25f) {
		discard;
	}
    return push.color;
}
