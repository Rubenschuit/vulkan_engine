// ------------- Graphics --------------

struct UniformBuffer {
	float4x4 view;
	float4x4 proj;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> g_ubo;

struct VertexInput {
	float4 pos : POSITION; // xyz = center, w = size
	float4 color : COLOR; // rgba
};

struct VertexOutput {
	float4 pos : SV_Position;
	float4 color : COLOR0;
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
VertexOutput vertMain(VertexInput input, uint id : SV_VertexID) {


	// Each instance renders 6 vertices (two triangles)
	uint corner = id % 6;

	float4 pos = float4(input.pos.x, input.pos.y, input.pos.z, 1.0);

	// Billboard in camera space by offsetting XY around the particle center
	float4 pos_cam_space = mul(g_ubo.view, pos);

	float2 offset = quad[corner] * input.pos.w; // size in input.pos.w
	pos_cam_space.xy += offset;

	VertexOutput out;
	out.pos = mul(g_ubo.proj, pos_cam_space);
	out.color = input.color;
	out.uv = quad[corner]; // pass for round mask and potential texture mapping
	return out;
}

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
	// Round particle (discard outside circle)
	if (dot(in_vert.uv, in_vert.uv) > 0.25f)
		discard;
	return in_vert.color;

}



// --------------- Compute ------------------



struct Params {
	float delta_time; // seconds
	float total_time; // seconds
	uint32_t particle_count;
	uint32_t reset; // 1 = reinitialize particles this dispatch
	uint32_t seed;  // rng seed for reset
	uint32_t reset_kind; // 1 = point, 2 = disc
	int mode; // 1=weirdOrbiting, 2=gravityFloor, 3=galaxy, 4=galaxyMassive
	float3 origin;
};
[vk::binding(3, 0)]
ConstantBuffer<Params> params;

struct Particle {
	float4 position; // w is size
	float4 velocity; // w unused
	float4 color;    // rgba
};
[vk::binding(1, 0)]
RWStructuredBuffer<Particle> particles_prev;
[vk::binding(2, 0)]
RWStructuredBuffer<Particle> particles_out;

// --------- RNG helpers ----------
uint wang_hash(uint seed) {
	seed = (seed ^ 61u) ^ (seed >> 16);
	seed *= 9u;
	seed = seed ^ (seed >> 4);
	seed *= 0x27d4eb2du;
	seed = seed ^ (seed >> 15);
	return seed;
}

float rand01(inout uint state) {
	state = wang_hash(state);
	// 24-bit mantissa to [0,1)
	return (float)(state & 0x00FFFFFFu) / 16777216.0f;
}

// Box-Muller to approximate normal distribution
float2 rand_normal2(inout uint state) {
	float mean = 0.0f;
	float stddev = 3.0f;
	float u1 = max(1e-6f, rand01(state));
	float u2 = rand01(state);
	float r = sqrt(-2.0f * log(u1));
	float theta = 6.28318530718f * u2;
	return float2(r * cos(theta), r * sin(theta)) * stddev + mean;
}

// ---------------- Simulation programs ----------------

// 1) Orbiting around a center with simple gravity + angular motion
// weird but cool behavior
Particle weirdOrbiting(Particle p) {
	float3 center = params.origin;
	float3 to_center = center - p.position.xyz;
	float dist = length(to_center) + 0.01f; // avoid div by zero
	float3 dir = to_center / dist;
	// Gravity towards center
	const float gravity_strength = 3.0f;
	p.velocity.xyz += dir * gravity_strength * params.delta_time;
	// Add some angular momentum
	float3 up = float3(0.0f, 1.0f, 0.0f);
	float3 right = normalize(cross(dir, up));
	float3 forward = normalize(cross(dir, right));
	const float rotation_strength = 10.0f;
	p.velocity.xyz += forward * rotation_strength * params.delta_time;
	// Integrate
	p.position.xyz += p.velocity.xyz * params.delta_time;
	return p;
}

// 2) Earth-like gravity and floor collision
Particle simulateGravityFloor(Particle p) {
	const float g = 9.81f;
	float3 accel = float3(0.0, 0.0, -g);
	p.velocity.xyz += accel * params.delta_time;
	p.position.xyz += p.velocity.xyz * params.delta_time;

	float floor_z = 0.1f;
	if (p.position.z < floor_z) {
		p.position.z = floor_z;
		if (p.velocity.z < 0.0f) {
			p.velocity.z = -p.velocity.z * 0.8f;
		}
		p.velocity.x *= 0.98f;
		p.velocity.y *= 0.98f;
	}
	return p;
}

// chatgpt generated code for:
// 3) Galaxy spiral: central gravity + tangential drive + spiral arm attraction
Particle simulateGalaxy(Particle p) {
	// Relative position in galaxy plane (XY), with origin as center
	float3 rel = p.position.xyz - params.origin;
	float2 relXY = rel.xy;
	float r = length(relXY);
	float eps = .9f; // softening to avoid singularity
	float rSafe = max(r, eps);
	float2 n = relXY / rSafe; // radial unit in plane
	float2 t = float2(-n.y, n.x); // tangential (prograde)

	// Central gravity (planar) ~ mu / r^2 towards center, plus mild vertical spring
	const float mu = 2.0f; // mass parameter
	float2 a_c = (-mu / (rSafe * rSafe)) * n;
	float a_z = (-mu * 0.02f) * rel.z; // weak spring to galactic plane

	// Desired tangential speed ~ sqrt(mu / r) (clamped at small r)
	float v_circ = sqrt(mu / rSafe);
	const float swirl_gain = 0.6f;
	float2 a_swirl = swirl_gain * v_circ * t / max(rSafe, 5.0f);

	// Spiral arms: attract towards logarithmic spiral loci
	// theta_arm = log(r) * tightness + omega * t; arms = K
	const float arms = 5.0f;
	const float tightness = 1.9f; // bigger -> tighter spiral
	const float omega = 0.2f;     // arm rotation speed (rad/s)
	float theta = atan2(rel.y, rel.x);
	float phase = theta - log(rSafe) * tightness - omega * params.total_time;
	float arm_signal = sin(arms * phase);
	float2 a_arms = -0.8f * arm_signal * n; // pull toward arm valleys

	// Sum accelerations
	float3 accel = float3(a_c + a_swirl + a_arms, a_z);

	// Damping to stabilize
	const float damping = 0.02f;
	p.velocity.xyz += accel * params.delta_time;
	p.velocity.xyz *= (1.0f - damping);

	// Integrate
	p.position.xyz += p.velocity.xyz * params.delta_time;
	return p;
}

// chatgpt generated code:
// ---- Massive galaxy  ----
float v_circ_flat(float r, float v0, float r0) {
	// Flat rotation curve: approaches v0, smoothed near center by r0
	return v0 * (1.0f - exp(-r / max(1e-3f, r0)));
}

Particle simulateGalaxyMassive(Particle p) {
	const float eps = 1.0f;    // core softening
	const float v0 = 35.0f;    // asymptotic circular speed
	const float r0 = 30.0f;    // rotation curve scale radius
	const float k_tan = 2.0f;  // tangential steering gain
	const float k_rad = 1.5f;  // radial damping gain
	const float k_z = 0.2f;    // vertical spring
	const float k_zv = 0.2f;   // vertical damping
	const float arms = 5.0f;   // spiral arms count
	const float tight = 1.3f;  // spiral tightness
	const float omega = 0.1f; // arm rotation speed
	const float arm_amp = 7.0f;// arm radial attraction

	float3 rel = p.position.xyz - params.origin;
	float2 relXY = rel.xy;
	float r = length(relXY);
	float rSafe = max(r, eps);
	float2 n = (rSafe > 0.0f) ? relXY / rSafe : float2(1.0f, 0.0f);
	float2 t = float2(-n.y, n.x);

	// Decompose planar velocity
	float2 vxy = p.velocity.xy;
	float vt = dot(vxy, t);
	float vr = dot(vxy, n);

	// Target circular speed (flat curve)
	float vt_target = v_circ_flat(rSafe, v0, r0);

	// Steer toward target tangential speed; damp radial motion
	vt += (vt_target - vt) * k_tan * params.delta_time;
	vr += (-vr) * k_rad * params.delta_time;

	// Spiral arms: rotating log-spiral phase -> radial attraction
	float theta = atan2(rel.y, rel.x);
	float phase = theta - log(rSafe) * tight - omega * params.total_time;
	float arm_signal = sin(arms * phase);
	float2 a_arm = -arm_amp * arm_signal * n;

	// Vertical spring + damping
	float az = -k_z * rel.z - k_zv * p.velocity.z;

	// Recombine planar velocity and integrate accelerations
	vxy = vt * t + vr * n;
	p.velocity.xy = vxy + a_arm * params.delta_time;
	p.velocity.z += az * params.delta_time;

	// Integrate position
	p.position.xyz += p.velocity.xyz * params.delta_time;
	return p;
}

void resetPoint(uint i) {
	uint state = (i + 1u) * 747796405u ^ params.seed;

		// Scale in [0.1, 0.2)
		float scale = 0.1f + 0.1f * rand01(state);

		// Position around (0,0,10) with small normal noise

		float3 pos = params.origin + float3(0.0f, 0.0f, 10.0f);
		pos += 0.1* rand_normal2(state).xyy; // small normal noise

		// Velocity random normal
		float2 vxy = rand_normal2(state);
		float vz = rand_normal2(state).x;
		float3 vel = float3(vxy.x, vxy.y, vz);

		// Color random in [0,1)
		float3 col = float3(rand01(state), rand01(state), rand01(state));

		Particle p0;
		p0.position = float4(pos, scale);
		p0.velocity = float4(vel, 0.0f);
		p0.color = float4(col, 1.0f);

		particles_out[i] = p0;
		particles_prev[i] = p0; // keep prev/current in sync
}

void resetDisc(uint i) {
	uint state = (i + 1u) * 747796405u ^ params.seed;

	// Scale in [0.08, 0.18)
	float scale = 0.08f + 0.10f * rand01(state);

	// Sample a wide disc (denser inner core): r ~ sqrt(U) * Rmax + Rmin
	const float Rmin = 3.0f;
	const float Rmax = 120.0f;
	float u = rand01(state);
	float r = sqrt(u) * Rmax + Rmin;
	// Align initial angle roughly along a spiral arm with noise
	const float tight = 2.0f;
	float baseTheta = rand01(state) * 6.2831853f;
	float theta0 = log(max(r, 1.0f)) * tight + baseTheta;
	float2 dir = float2(cos(theta0), sin(theta0));
	float3 pos = float3(params.origin.xy + dir * r, params.origin.z + (rand01(state) - 0.5f) * 2.0f);

	// Initialize near-circular tangential velocity with slight noise
	float vt0 = v_circ_flat(r, 35.0f, 30.0f);
	float2 t = float2(-dir.y, dir.x);
	float2 vxy = vt0 * t + (rand_normal2(state) * 0.2f);
	float vz = (rand01(state) - 0.5f) * 0.2f;
	float3 vel = float3(vxy.x, vxy.y, vz);

	// Color by radius gradient
	float3 col = lerp(float3(1.0f, 0.9f, 0.8f), float3(0.6f, 0.8f, 1.0f), saturate(r / Rmax));

	Particle p0;
	p0.position = float4(pos, scale);
	p0.velocity = float4(vel, 0.0f);
	p0.color = float4(col, 1.0f);

	particles_out[i] = p0;
	particles_prev[i] = p0; // keep prev/current in sync
}

[shader("compute")]
[numthreads(256, 1, 1)]
void compMain(uint3 thread_id : SV_DispatchThreadID) {
	uint i = thread_id.x;
	if (i >= params.particle_count)
		return;

	// Reset particles
	if (params.reset != 0u) {
		if (params.reset_kind == 2u) {
			resetDisc(i);
		} else {
			resetPoint(i);
		}
		return;
	}

	Particle p = particles_prev[i];

	// Choose simulation based on mode
	switch (params.mode) {
		case 1:
			p = weirdOrbiting(p);
			break;
		case 2:
			p = simulateGravityFloor(p);
			break;
		case 3:
			p = simulateGalaxy(p);
			break;
		case 4:
		default:
			p = simulateGalaxyMassive(p);
			break;
	}
	particles_out[i] = p;
}
