// Particle main-update compute kernel.
//
// Reads particles_prev (last frame's state), applies physics and
// writes particles_out, appends visible survivors to a render buffer. Dead
// particles are stamped with the free-list sentinel in both ping-pongs and
// queued onto recycle_queue for particle_recycle_comp.

#include "particle_common.slangh"

// Applies physics
Particle simulateParametric(Particle p, uint emitter_id) {
	EmitterParams e = emitter_params[emitter_id];

	float3 accel = float3(0.0f, 0.0f, -e.gravity);

	// Central attractor (toward origin) + tangential force around up axis
	if (e.central_attractor != 0.0f || e.tangential_strength != 0.0f) {
		float3 to_center = e.origin.xyz - p.position.xyz;
		float dist = length(to_center) + 0.01f; // avoid div by zero
		float3 dir = to_center / dist;
		if (e.central_attractor != 0.0f)
			accel += dir * e.central_attractor;
		if (e.tangential_strength != 0.0f) {
			float3 up = float3(0.0f, 1.0f, 0.0f);
			float3 right = normalize(cross(dir, up));
			float3 forward = normalize(cross(dir, right));
			accel += forward * e.tangential_strength;
		}
	}

	p.velocity.xyz += accel * params.delta_time;

	// Linear drag
	if (e.drag != 0.0f)
		p.velocity.xyz *= (1.0f - e.drag * params.delta_time);

	// Wind blows particles without affecting their stored velocity (so drag
	// doesn't decay the wind contribution). e.wind.xyz is normalized CPU-side.
	float3 wind_velocity = e.wind.xyz * e.wind.w;
	p.position.xyz += (p.velocity.xyz + wind_velocity) * params.delta_time;

	// Optional bounce floor (floor_z <= -1e30 disables)
	if (p.position.z < e.floor_z) {
		p.position.z = e.floor_z;
		if (p.velocity.z < 0.0f)
			p.velocity.z = -p.velocity.z * 0.8f;
		p.velocity.x *= 0.98f;
		p.velocity.y *= 0.98f;
	}
	return p;
}

// Spawn a child particle of `child_emitter_id` at the parent's location. Used
// by the generic on_tick / on_tick2 sub-emitter paths. Writes directly to
// particles_out[child_index]. Drops the spawn when the free list is exhausted.
void spawnChild(Particle parent, uint child_emitter_id) {
	uint child_index;
	if (!popFreeIndex(child_index))
		return;

	EmitterParams ce = emitter_params[child_emitter_id];
	uint state = wang_hash(child_index ^ asuint(params.total_time));

	// Can inherit from parent. Scale: 0 = inherit, >0 = absolute.
	float child_scale = (ce.spawn_scale > 0.0f) ? ce.spawn_scale : parent.position.w;

	// Life: spawn_life_fraction=0 means use child's min/max_life range.
	// Otherwise inherit fraction of parent's life.
	float child_life;
	if (ce.spawn_life_fraction > 0.0f) {
		child_life = parent.velocity.w * ce.spawn_life_fraction;
	} else {
		float u = rand01(state);
		child_life = ce.min_life + (ce.max_life - ce.min_life) * u;
	}

	float trail_phase = ce.trail_interval * rand01(state);

	Particle child;
	child.position = float4(parent.position.xyz, child_scale);
	child.velocity = float4(parent.velocity.xyz * ce.spawn_velocity_scale, child_life);
	child.color = parent.color * ce.color_start;
	child.color.rgb *= ce.brightness;
	child.tex_coords = float4(0.0f);
	child.atlas_index = ce.atlas_index;
	child.max_life = child_life;
	child.blend = 0.0f;
	child.emitter_id = child_emitter_id;
	child.trail_accumulator = trail_phase;
	child.spawn_scale = child_scale;
	child._pad0 = 0.0f;
	child._pad1 = 0.0f;

	particles_out[child_index] = child;
	appendToRenderBuffer(child);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void compMain(uint3 thread_id : SV_DispatchThreadID) {
	uint i = thread_id.x;
	if (i >= params.capacity)
		return;

	if (params.reset != 0u) {
		Particle p = makeFreeSlot();
		particles_out[i] = p;
		particles_prev[i] = p;
		return;
	}

	Particle p = particles_prev[i];

	if (inFreeList(p))
		return;

	uint emitter_id = p.emitter_id;

	p.velocity.w -= params.delta_time;

	// Stamp the free-list sentinel into BOTH ping-pong buffers
	if (p.velocity.w <= 0.0f) {
		Particle freed = makeFreeSlot();
		particles_out[i] = freed;
		particles_prev[i] = freed;
		addToRecycleQueue(i);
		return;
	}


	EmitterParams e = emitter_params[emitter_id];
	p = simulateParametric(p, emitter_id);

	// Envelopes over life (life_frac=1 at spawn, 0 at death).
	if (p.max_life > 1e-3f) {
		float life_frac = saturate(p.velocity.w / p.max_life);

		// Color: lerp color_start (life_frac=1) -> color_end (life_frac=0).
		{
			float bright = e.brightness;
			float4 cs = float4(e.color_start.rgb * bright, e.color_start.a);
			float4 ce = float4(e.color_end.rgb * bright, e.color_end.a);
			float4 diff = abs(cs - ce);
			float4 curve = lerp(ce, cs, life_frac);
			if (diff.r > 1e-4f) p.color.r = curve.r;
			if (diff.g > 1e-4f) p.color.g = curve.g;
			if (diff.b > 1e-4f) p.color.b = curve.b;
			if (diff.a > 1e-4f) p.color.a = curve.a;
		}

		// Scale size over life if enabled
		if (e.scale_end >= 0.0f)
			p.position.w = lerp(e.scale_end, p.spawn_scale, life_frac);
	}

	// On-tick sub-emitter chaining.
	if (e.on_tick_emitter_id >= 0 || e.on_tick2_emitter_id >= 0) {
		p.trail_accumulator -= params.delta_time;
		int spawn_count = 0;
		const int MAX_SPAWNS_PER_FRAME = 8;
		while (p.trail_accumulator <= 0.0f && spawn_count < MAX_SPAWNS_PER_FRAME) {
			p.trail_accumulator += e.trail_interval;
			spawn_count++;
			uint state = wang_hash(i ^ asuint(params.total_time) ^ uint(spawn_count));
			if (e.on_tick_emitter_id >= 0 && rand01(state) < e.tick_probability)
				spawnChild(p, (uint)e.on_tick_emitter_id);
			if (e.on_tick2_emitter_id >= 0 && rand01(state) < e.tick2_probability)
				spawnChild(p, (uint)e.on_tick2_emitter_id);
		}
	}

	// update tex_coords
	int num_rows = max(1, (int)e.row_count);
	int stage_count = num_rows * num_rows;

	float cycle_progress = 0.0f;
	bool one_shot = (e.atlas_one_shot != 0u);

	if (one_shot) {
		// One-shot atlas: progress tied to lifetime, 0..1 over the particle's life.
		if (p.max_life > 0.001f) {
			cycle_progress = 1.0f - (p.velocity.w / p.max_life);
		} else {
			cycle_progress = 1.0f;
		}
		cycle_progress = saturate(cycle_progress);
	} else {
		// Looping atlas: time-based cycling per particle (seeded by scale for variance).
		float cycle_time = 1.0f;
		float flipbook_seed = p.position.w * 9.9f;
		cycle_progress = ((params.total_time + flipbook_seed) % cycle_time) / cycle_time;
	}

	float atlas_progression = cycle_progress * (float)stage_count;

	// lerp between two adjacent stages
	int index1 = (int)floor(atlas_progression);
	int index2 = index1 + 1;

	// One-shot clamps to last frame; looping wraps to 0.
	if (one_shot) {
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

	// One-shot clamped at last frame: collapse blend.
	if (one_shot && index1 == stage_count - 1) {
		blend = 0.0f;
	}


	int column1 = index1 % num_rows;
	int column2 = index2 % num_rows;
	int row1 = index1 / num_rows;
	int row2 = index2 / num_rows;

	p.tex_coords = float4(column1, row1, column2, row2) / (float)num_rows;
	p.blend = blend;

	particles_out[i] = p;

	appendToRenderBuffer(p);
}