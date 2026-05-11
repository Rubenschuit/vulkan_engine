// Debug visualization: draws the skin position buffer as colored points.

struct VertexInput {
	float3 in_pos : POSITION;
};

struct GlobalUbo {
	float4x4 view;
	float4x4 proj;
};
[[vk::binding(0, 0)]] ConstantBuffer<GlobalUbo> ubo;

struct DebugPush {
	float4x4 model;
	float4 color;     // rgb = point color, a = point pixel size
};
[[vk::push_constant]] DebugPush push;

struct VertexOutput {
	float4 pos : SV_Position;
	float pt_size : SV_PointSize;
	float3 color;
};

struct FragmentInput {
	float4 pos : SV_Position;
	float3 color;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input) {
	VertexOutput o;
	float4 world = mul(push.model, float4(input.in_pos, 1.0));
	o.pos = mul(ubo.proj, mul(ubo.view, world));
	o.pt_size = push.color.a;
	o.color = push.color.rgb;
	return o;
}

[shader("fragment")]
float4 fragMain(FragmentInput inV) : SV_Target {
	return float4(inV.color, 1.0);
}