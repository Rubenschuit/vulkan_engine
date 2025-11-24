// ------------- Graphics --------------

struct UniformBuffer {
	float4x4 view;
	float4x4 proj;
	float4 camera_position;
};
[vk::binding(0, 0)]
ConstantBuffer<UniformBuffer> g_ubo;

struct VertexInput {
	float4 pos : POSITION; // xyz = center, w = size
	float4 color : COLOR; // rgba
	float4 tex_coords : TEXCOORD; // uv1 and uv2
	float4 extra_data;  // x = type, y = lifetime, z = blend
};

struct VertexOutput {
	float4 pos : SV_Position;
	float4 color : COLOR0;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float2 uv2 : TEXCOORD2;
	nointerpolation uint type : TEXCOORD3;
	float blend : TEXCOORD4;
};

static const float2 quad[6] = {
	float2( 0.5,  0.5),
	float2(-0.5, -0.5),
	float2(-0.5,  0.5),

	float2( 0.5, -0.5),
	float2(-0.5, -0.5),
	float2( 0.5,  0.5)
};

enum ParticleType : uint {
	TYPE_DEFAULT = 0,
	TYPE_SMOKE = 1,
	TYPE_SPARK = 2,
	TYPE_ROCKET = 3,
	TYPE_EXPLOSION = 4,
	TYPE_EXPLOSION_SMOKE = 5
};


[shader("vertex")]
VertexOutput vertMain(VertexInput input, uint id : SV_VertexID) {

	// Each instance renders 6 vertices (two triangles)
	uint corner = id % 6;

	float4 pos = float4(input.pos.xyz, 1.0);
	float4 pos_cam_space = mul(g_ubo.view, pos);

	// Calculate scale based on camera distance to keep size consistent,
	// prevents from disappearing too quickly when far away
	float scale = input.pos.w; // base size
	float distance = length(pos.xyz - g_ubo.camera_position.xyz);
	float distance_scale = 1.0f; // scale factor
	uint type = (uint)input.extra_data.x;

	// scale smoothly after 100 units
	// Skip scaling for rockets (these can be very large when they explode)
	if (distance > 100.0f && type != ParticleType::TYPE_ROCKET) {
		distance_scale = distance * 0.005f + 0.5f;
	}

	// Billboard in camera space by offsetting XY around the particle center
	float2 offset = quad[corner] * scale * distance_scale;
	pos_cam_space.xy += offset;

	VertexOutput out;
	out.pos = mul(g_ubo.proj, pos_cam_space);
	out.color = input.color;
	// pass through tex_coords for each corner
	float2 uv = quad[corner] + 0.5f;
	//uv.x = 1.0f - uv.x;
	uv.y = 1.0f - uv.y;

	uv /= 8.0f;

	out.uv0 = quad[corner];
	out.uv1 = uv + input.tex_coords.xy;
	out.uv2 = uv + input.tex_coords.zw;
	out.type = type;
	out.blend = input.extra_data.z;
	return out;
}

[vk::binding(0, 1)]
Sampler2D glow_texture;
[vk::binding(1, 1)]
Sampler2D fire_texture;
[vk::binding(2, 1)]
Sampler2D smoke_texture;

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
	float4 col1, col2;
	if (in_vert.type == ParticleType::TYPE_DEFAULT) {
		// round mask
		if (dot(in_vert.uv0, in_vert.uv0) > 0.25f) {
			discard;
		}
		// For additive blending, we must pre-multiply alpha to affect RGB intensity
		return float4(in_vert.color.rgb * in_vert.color.a, in_vert.color.a);
	} else if (in_vert.type == ParticleType::TYPE_SMOKE || in_vert.type == ParticleType::TYPE_EXPLOSION_SMOKE) {
		col1 = smoke_texture.Sample(in_vert.uv1);
		col2 = smoke_texture.Sample(in_vert.uv2);
	} else {
		col1 = fire_texture.Sample(in_vert.uv1);
		col2 = fire_texture.Sample(in_vert.uv2);
	}
	float4 col = lerp(col1, col2, in_vert.blend);
	if (col.a <= 0.01f)
		discard;

	float4 final_col = col * in_vert.color;
	return float4(final_col.rgb * final_col.a, final_col.a);
}


// --------------- Compute shader------------------




struct SpawnEvent {
	float4 position_scale; // xyz = position, w = scale
	float4 velocity_life;     // xyz = velocity, w = life
	float4 color;             // rgba
	uint4 info;             // x = start_index, y = count, z = type, w = variance (as float bits)
};

struct Params {
	float delta_time; // seconds
	float total_time; // seconds
	uint32_t particle_count;
	uint32_t reset; // 1 = reinitialize particles this dispatch
	uint32_t seed;  // rng seed for reset
	float mean;
	float stddev;
	uint32_t reset_kind; // 1 = point, 2 = disc
	int mode; // 1,2,3,4,5 see particle_system.hpp ParticleMode enum
	float4 origin;
	uint32_t row_count;
	float min_life;
	float max_life;
	uint32_t should_respawn;

	uint32_t spawn_event_count;
	float gravity;
	uint32_t padding1;
	uint32_t padding2;
	float4 wind_direction; // xyz = direction, w = intensity
};
[vk::binding(3, 0)]
ConstantBuffer<Params> params;

[vk::binding(4, 0)]
StructuredBuffer<SpawnEvent> spawn_events;

struct Particle {
	float4 position; // w is size
	float4 velocity; // w unused
	float4 color;    // rgba
	float4 tex_coords; // uv1 and uv2
	float4 extra_data; // x = type
};
[vk::binding(1, 0)]
RWStructuredBuffer<Particle> particles_prev;
[vk::binding(2, 0)]
RWStructuredBuffer<Particle> particles_out;

// RNG helpers
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
	float mean = params.mean;
	float stddev = params.stddev;
	float u1 = max(1e-6f, rand01(state));
	float u2 = rand01(state);
	float r = sqrt(-2.0f * log(u1));
	float theta = 6.28318530718f * u2;
	return float2(r * cos(theta), r * sin(theta)) * stddev + mean;
}

// ---------------- Simulation functions ----------------

// 1) Earth-like gravity and floor collision
Particle simulateGravityFloor(Particle p) {
	float3 accel = float3(0.0, 0.0, -params.gravity);
	p.velocity.xyz += accel * params.delta_time;
	p.position.xyz += p.velocity.xyz * params.delta_time;

	float floor_z = 0.5f;
	if (p.position.z < floor_z) {
		p.position.z = floor_z;
		if (p.velocity.z < 0.0f) {
			p.velocity.z = -p.velocity.z * 0.8f;
		}
		p.velocity.x *= 0.98f; // friction
		p.velocity.y *= 0.98f;
	}
	return p;
}

// 2) Orbiting around a center with simple gravity + angular motion
// weird but cool behavior
Particle coolOrbit(Particle p) {
	float3 center = params.origin.xyz;
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
	p.position.xyz += p.velocity.xyz * params.delta_time;
	return p;
}

// 3) Simple gravity towards center
Particle succ(Particle p) {
	float3 center = params.origin.xyz;
	float3 to_center = center - p.position.xyz;
	float dist = length(to_center) + 0.01f; // avoid div by zero
	float3 dir = to_center / dist;
	// Gravity towards center
	const float gravity_strength = 26.0f;
	p.velocity.xyz += dir * gravity_strength * params.delta_time;

	p.position.xyz += p.velocity.xyz * params.delta_time;
	return p;
}

// chatgpt generated code (terrible):
// 4) Attempt at a galaxy but puts particle in some kind of stasis
Particle simulateStasis(Particle p) {
	// Relative position in galaxy plane (XY), with origin as center
	float3 rel = p.position.xyz - params.origin.xyz;
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

// chatgpt generated code (not good):
// 5) Massive galaxy

// helper
float v_circ_flat(float r, float v0, float r0) {
	// Flat rotation curve: approaches v0, smoothed near center by r0
	return v0 * (1.0f - exp(-r / max(1e-3f, r0)));
}

// Looks a little bit like a galaxy but is not stable
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

	float3 rel = p.position.xyz - params.origin.xyz;
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



// ----------- Particle reset functions ------------



// Particles explode in random directions from a point
void resetPoint(uint i, uint extra_seed) {
	uint state = (i + 1u) * 747796405u ^ params.seed ^ extra_seed;

	// Scale in [0.1, 0.2)
	float scale = 0.1f + 0.1f * rand01(state);

	// Position around (0,0,10) with small normal noise

	float3 pos = params.origin.xyz;
	pos += 0.1 * rand_normal2(state).xyy; // small normal noise

	// Velocity random normal
	float2 vxy = rand_normal2(state);
	float vz = rand_normal2(state).x;

	// Initialize life in w component
	float life = params.min_life + (params.max_life - params.min_life) * rand01(state);
	float3 vel = float3(vxy.x, vxy.y, vz);

	// Color random in [0,1)
	float3 col = float3(rand01(state), rand01(state), rand01(state));

	Particle p0;
	p0.position = float4(pos, scale);
	p0.velocity = float4(vel, life);
	p0.color = float4(col, 1.0f);
	p0.tex_coords = float4(0.0f);
	p0.extra_data = float4(0.0f);

	particles_out[i] = p0;
	particles_prev[i] = p0;
}

// Particles initialized in a rotating disc
void resetDisc(uint i, uint extra_seed) {
	uint state = (i + 1u) * 747796405u ^ params.seed ^ extra_seed;

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

	// Initialize tangential velocity with slight noise
	float vt0 = v_circ_flat(r, 35.0f, 30.0f);
	float2 t = float2(-dir.y, dir.x);
	float2 vxy = vt0 * t + (rand_normal2(state) * 0.2f);
	float vz = (rand01(state) - 0.5f) * 0.2f;

	// Initialize life in w component
	float life = params.min_life + (params.max_life - params.min_life) * rand01(state);
	float3 vel = float3(vxy.x, vxy.y, vz);

	// Color by radius gradient
	float3 col = lerp(float3(1.0f, 0.9f, 0.8f), float3(0.6f, 0.8f, 1.0f), saturate(r / Rmax));

	Particle p0;
	p0.position = float4(pos, scale);
	p0.velocity = float4(vel, life);
	p0.color = float4(col, 1.0f);
	p0.tex_coords = float4(0.0f);
	p0.extra_data = float4(0.0f);

	particles_out[i] = p0;
	particles_prev[i] = p0;
}

void resetFromEvent(uint i, SpawnEvent e) {
	uint seed = asuint(params.total_time) ^ i ^ e.info.x;
	uint state = wang_hash(seed);

	float4 pos_scale = e.position_scale; // xyz = pos, w = scale
	float4 vel_life = e.velocity_life; // xyz = vel, w = life
	float4 color = e.color;
	uint type = e.info.z;

	float variance = asfloat(e.info.w);

	// Apply variance to position
	if (variance > 0.0f) {
		pos_scale.xyz += variance * rand_normal2(state).xyy;
	}

	float3 final_vel = vel_life.xyz;
	float life = vel_life.w;

	// Add lifetime variance
	if (type != ParticleType::TYPE_ROCKET) {
		life *= (0.8f + 0.4f * rand01(state));
	}

	// Logic per type
	if (type == ParticleType::TYPE_DEFAULT) {
		 // Legacy / Default
		 // If initial vel is zero, add random velocity like resetPoint
		 if (length(final_vel) < 0.001f) {
			 float2 vxy = rand_normal2(state);
			 float vz = rand_normal2(state).x;
			 final_vel = float3(vxy.x, vxy.y, vz);
		 }
		 if (vel_life.w <= 0.0f) { // Use base life if <= 0 provided
			 life = params.min_life + (params.max_life - params.min_life) * rand01(state);
		 }
		 color.rgb *= (0.8f + 0.2f * rand01(state)); // slight color variance
	} else if (type == ParticleType::TYPE_SPARK) { // Spark / Firework Star
		 // Random burst velocity added to base velocity
		 float2 vxy = rand_normal2(state);
		 float vz = rand_normal2(state).x;
		 float speed = 1.0f + 1.0f * rand01(state);
		 final_vel += float3(vxy.x, vxy.y, vz) * speed;

		 // More lifetime variance for sparks
		 if (vel_life.w <= 0.0f) life = 0.5f + rand01(state) * 1.5f;
		 else life = vel_life.w * (0.5f + 1.0f * rand01(state));

	} else if (type == ParticleType::TYPE_EXPLOSION) { // Main explosion
		 // High velocity burst
		 float2 vxy = rand_normal2(state);
		 float vz = rand_normal2(state).x;
		 float speed = 0.5f + 3.0f * rand01(state);
		 final_vel += float3(vxy.x, vxy.y, vz) * speed;

		 if (vel_life.w <= 0.0f) life = 0.5f + rand01(state) * 0.5f;
		 else life = vel_life.w * (0.8f + 0.4f * rand01(state));

	} else if (type == ParticleType::TYPE_EXPLOSION_SMOKE) { // Smoke from explosion
		 // High velocity burst
		 float2 vxy = rand_normal2(state);
		 float vz = rand_normal2(state).x;
		 float speed = 0.5f + 3.0f * rand01(state);
		 final_vel += float3(vxy.x, vxy.y, vz) * speed;

		 if (vel_life.w <= 0.0f) life = 0.5f + rand01(state) * 0.5f;
		 else life = vel_life.w * (0.8f + 0.4f * rand01(state));
	} else if (type == ParticleType::TYPE_ROCKET) { // Rocket
		 // Minimal noise
		 final_vel += rand_normal2(state).xyy * 0.1f;
		 if (vel_life.w <= 0.0f) life = 2.0f;
		 else life = vel_life.w; // Rockets usually fixed life
	} else if (type == ParticleType::TYPE_SMOKE) { // Smoke
		 // Slow random drift
		 final_vel += rand_normal2(state).xyy * 0.05f;
		 if (vel_life.w <= 0.0f) life = 1.0f + rand01(state);
		 else life = vel_life.w * (0.7f + 0.6f * rand01(state));
	}

	float scale = pos_scale.w;
	if (scale <= 0.0f) scale = 0.1f + 0.1f * rand01(state); // Fallback if scale 0

	Particle p;
	p.position = float4(pos_scale.xyz, scale);
	p.velocity = float4(final_vel, life);
	p.color = color;
	p.tex_coords = float4(0.0f);
	// Store initial life in extra_data.y for texture animation
	p.extra_data = float4((float)type, life, 0, 0);

	particles_out[i] = p;
	particles_prev[i] = p;
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
			resetDisc(i, 0);
		} else {
			resetPoint(i, 0);
		}
		return;
	}

	// Initialize emitted particles
	for (uint k = 0; k < params.spawn_event_count; ++k) {
		SpawnEvent e = spawn_events[k];
		if (i >= e.info.x && i < e.info.x + e.info.y) {
			 resetFromEvent(i, e);
			 return;
		}
	}

	Particle p = particles_prev[i];

	// Update lifetime
	p.velocity.w -= params.delta_time;
	if (p.velocity.w <= 0.0f) {
		if (params.should_respawn != 0u) {
			// Respawn with a seed based on time to vary the randomness
			uint time_seed = asuint(params.total_time);
			if (params.reset_kind == 2u) {
				resetDisc(i, time_seed);
			} else {
				resetPoint(i, time_seed);
			}
			return;
		} else {
			// Do not respawn, make invisible
			p.position.w = 0.0f;
		}
	}

	// Physics Update
	uint type = (uint)p.extra_data.x;
	if (type == ParticleType::TYPE_DEFAULT) {
		// Choose simulation based on mode
		switch (params.mode) {
			case 1:
				p = simulateGravityFloor(p);
				break;
			case 2:
				p = coolOrbit(p);
				break;
			case 3:
				p = succ(p);
				break;
			case 4:
				p = simulateStasis(p);
				break;
			default:
				p = simulateGalaxyMassive(p);
				break;
		}
	} else { // Firework particles
		float gravity = params.gravity;
		if (type == ParticleType::TYPE_SMOKE || type == ParticleType::TYPE_EXPLOSION_SMOKE || type == ParticleType::TYPE_EXPLOSION) {
			gravity = -2.0f; // Buoyancy for smoke
		}

		p.velocity.z -= gravity * params.delta_time;

		if (type == ParticleType::TYPE_SMOKE || type == ParticleType::TYPE_EXPLOSION_SMOKE) { // Smoke
			 p.velocity.xyz -= p.velocity.xyz * .9f * params.delta_time; // Drag applies only to internal velocity
		} else if (type == ParticleType::TYPE_SPARK) { // Spark
			 p.velocity.xyz -= p.velocity.xyz * 0.8f * params.delta_time; // Drag
			 float max_life = p.extra_data.y;
			 if (max_life > 0.001f) {
				 float life_frac = saturate(p.velocity.w / max_life);
				 float age = 1.0f - life_frac;
				 // Yellow -> Orange -> Red
				 p.color.rgb = lerp(float3(1.0f, 0.8f, 0.1f), float3(1.0f, 0.0f, 0.0f), age);
				 // Fade out in last 20% of life
				 p.color.a = saturate(life_frac / 0.2f);
			 }
		} else if (type == ParticleType::TYPE_EXPLOSION) { // Explosion
			 p.velocity.xyz -= p.velocity.xyz * 0.9f * params.delta_time; // Drag (air resistance)
			 // Fade based on lifetime fraction
			 float max_life = p.extra_data.y;
			 if (max_life > 0.001f) {
				 p.color.a = saturate(p.velocity.w / max_life);
			 } else {
				 p.color.a = 0.0f;
			 }
		} else if (type == ParticleType::TYPE_ROCKET) {
			 // Flash effect near end of life ( life < FLASH_TIME or z < -5.0f)
			 const float FLASH_SCALE = 150.0f;
			 const float FLASH_TIME = 0.15f;

			 // If falling too fast, force life to end to trigger flash
			 if (p.velocity.z < -5.0f && p.velocity.w > FLASH_TIME) {
				p.velocity.w = FLASH_TIME;
			 }

			 float life = p.velocity.w;
			 // Allow one frame of grace period after death (life <= 0) to show the peak flash
			 if (life <= FLASH_TIME && life > -params.delta_time * 2.0f) {
				 // Clamp t to [0, 1] to ensure we don't get negative scale or overshoot
				 float t = saturate(life / FLASH_TIME);
				 float flash_scale = FLASH_SCALE * t;
				 p.position.w = flash_scale;
				 p.color.a = 0.6f;
			 } else {
				 p.position.w = 0.0f;
			 }
		}

		// Apply wind force constantly (not affected by drag in this simple model, or add it to position directly)
		// Adding to position directly is better to keep "velocity" as the impulsive forces that decay.
		// Wind direction: params.wind_direction.xyz, intensity: params.wind_direction.w
		float3 wind_velocity = normalize(params.wind_direction.xyz) * params.wind_direction.w;

		// Update position with internal velocity + wind
		p.position.xyz += (p.velocity.xyz + wind_velocity) * params.delta_time;

		if (p.position.z < 0.0f) p.position.z = 0.0f;
	}



	// update tex_coords
	int num_rows = max(1, (int)params.row_count);
	int stage_count = num_rows * num_rows;

	float cycle_progress = 0.0f;
	uint particle_type = (uint)p.extra_data.x;

	if (particle_type == ParticleType::TYPE_SMOKE || particle_type == ParticleType::TYPE_EXPLOSION_SMOKE) {
		// For smoke, animation is tied to lifetime (extra_data.y is max_life)
		// Progress goes from 0.0 (start) to 1.0 (end)
		float max_life = p.extra_data.y;
		if (max_life > 0.001f) {
			cycle_progress = 1.0f - (p.velocity.w / max_life);
		} else {
			cycle_progress = 1.0f;
		}
		cycle_progress = saturate(cycle_progress);
		p.color.a = 0.4f;
	} else {
		// other types: use time-based cycling
		float cycle_time = 1.0f;
		float flipbook_seed = p.position.w * 9.9f; // scale as seed
		cycle_progress = ((params.total_time + flipbook_seed) % cycle_time) / cycle_time;
	}

	float atlas_progression = cycle_progress * (float)stage_count;

	// lerp between two adjacent stages
	int index1 = (int)floor(atlas_progression);
	int index2 = index1 + 1;

	// For smoke (one-shot), clamp to last frame instead of wrapping
	if (particle_type == ParticleType::TYPE_SMOKE || particle_type == ParticleType::TYPE_EXPLOSION_SMOKE) {
		if (index1 >= stage_count - 1) {
			index1 = stage_count - 1;
			index2 = stage_count - 1;
		} else if (index2 >= stage_count) {
			index2 = stage_count - 1;
		}
	} else {
		if (index2 >= stage_count) {
			index2 = 0;
		}
	}

	// take decimal part of atlas_progression
	float blend = atlas_progression - floor(atlas_progression);

	// If smoke is clamped at end, blend should be 0 or irrelevant
	if ((particle_type == ParticleType::TYPE_SMOKE || particle_type == ParticleType::TYPE_EXPLOSION_SMOKE) && index1 == stage_count - 1) {
		blend = 0.0f;
	}


	int column1 = index1 % num_rows;
	int column2 = index2 % num_rows;
	int row1 = index1 / num_rows;
	int row2 = index2 / num_rows;

	p.tex_coords = float4(column1, row1, column2, row2) / (float)num_rows;
	p.extra_data.z = blend;

	particles_out[i] = p;
}
