// ------------- Graphics --------------

struct UniformBuffer {
	float4x4 view;
	float4x4 proj;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> g_ubo;

struct VSOut {
	float4 pos : SV_Position;
	float3 color : COLOR0;
	float2 uv : TEXCOORD0; // offset within quad, used for round mask
};

static const float2 quad[6] = {
	float2(-0.5, -0.5),
	float2( 0.5, -0.5),
	float2( 0.5,  0.5),
	float2(-0.5, -0.5),
	float2( 0.5,  0.5),
	float2(-0.5,  0.5)
};

[shader("vertex")]
VSOut vertMain( [[vk::location(0)]] float4 inst_pos : POSITION,
				[[vk::location(1)]] float4 inst_color : COLOR,
				uint id : SV_VertexID,
				uint inst : SV_InstanceID) {


	// Each instance renders 6 vertices (two triangles)
	uint corner = id % 6;

	float4 pos = float4(inst_pos.x, inst_pos.y, inst_pos.z, 1.0);

	// Billboard in camera space by offsetting XY around the particle center
	float4 pos_cam_space = mul(g_ubo.view, pos);

	float2 offset = quad[corner] * inst_pos.w; // size in inst_pos.w
	pos_cam_space.xy += offset;

	VSOut out;
	out.pos = mul(g_ubo.proj, pos_cam_space);
	out.color = inst_color.xyz;
	out.uv = quad[corner]; // pass for round mask
	return out;
}

[shader("fragment")]
float4 fragMain(VSOut in_vert) : SV_Target {
	// Round particle (discard outside circle)
	if (dot(in_vert.uv, in_vert.uv) > 0.25f)
		discard;
	return float4(in_vert.color, 0.9f);

}



// --------------- Compute ------------------



struct Params {
	float delta_time; // seconds
	float total_time; // seconds
	uint32_t particle_count;
};
[vk::binding(3, 0)]
ConstantBuffer<Params> c_params;

struct Particle {
	float4 position; // w is size
	float4 velocity; // w unused
	float4 color;    // rgba
};
[vk::binding(1, 0)]
StructuredBuffer<Particle> particles_in;
[vk::binding(2, 0)]
RWStructuredBuffer<Particle> particles_out;

[shader("compute")]
[numthreads(256, 1, 1)]
void compMain(uint3 thread_id : SV_DispatchThreadID) {
	uint i = thread_id.x;
	if (i >= c_params.particle_count)
		return;

	Particle p = particles_in[i];


	// ------Particles attracted to center with some orbiting motion-----


	//the particles explode and then orbit a center (0,0,20)
	float3 to_center = - p.position.xyz + float3(0.0f, 0.0f, +20.0f);
	float dist = length(to_center) + 0.01f; // avoid div by zero
	float3 dir = to_center / dist;
	// Add some "gravity" towards center
	float gravity_strength = 5.0f;
	p.velocity.xyz += dir * gravity_strength * c_params.delta_time;

	float3 up = float3(0.0f, 1.0f, 0.0f);
	float3 right = normalize(cross(dir, up));
	float3 forward = normalize(cross(dir, right));
	float rotation_strength = 2.0f;
	p.velocity.xyz += (right * sin(c_params.total_time) + forward * cos(c_params.total_time)) * rotation_strength * c_params.delta_time;
	// Move by velocity
	p.position.xyz += p.velocity.xyz * c_params.delta_time;


	/* ------Simple earth-like gravity and floor collision-----

	// Integrate simple gravity (Z is world up)
	const float g = 9.81f;
	float3 accel = float3(0.0, 0.0, -g);
	p.velocity.xyz += accel * c_params.delta_time;

	// Update position with new velocity
	p.position.xyz += p.velocity.xyz * c_params.delta_time;

	float floor_z = 0.1f;
	if (p.position.z < floor_z) {
		// clamp to the plane
		p.position.z = floor_z;
		// reflect with damping
		if (p.velocity.z < 0.0f) {
			p.velocity.z = -p.velocity.z * 0.8f;
		}
		// simple ground friction
		p.velocity.x *= 0.98f;
		p.velocity.y *= 0.98f;
	}
	*/

	particles_out[i] = p;
}
