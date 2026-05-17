// Particle spawn pre-pass.
//
// Runs before particle_update_comp. For each pending
// SpawnEvent it pops `count` slots from the free list, builds a fresh
// Particle from the event + per-emitter parameters, and writes it directly
// to particles_out[slot].
//
// One workgroup per SpawnEvent, threads in the workgroup loop over the 
// event's particle count.

#include "particle_common.slangh"

Particle buildParticleFromEvent(SpawnEvent e, uint event_id, uint sub_index) {
	uint emitter_id = e.emitter_id;
	EmitterParams ep = emitter_params[emitter_id];
	uint state = wang_hash(asuint(params.total_time) ^ (event_id * 73856093u) ^ sub_index);

	float4 pos_scale = e.position_scale;
	float4 vel_life = e.velocity_life;
	float4 color = e.color;
	float3 final_vel = vel_life.xyz;
	float life = vel_life.w;


	if (ep.spawn_distribution == SPAWN_DIST_UNIFORM_SPHERE) {
		float2 dxy = rand_unit_normal2(state);
		float  dz  = rand_unit_normal2(state).x;
		float3 raw = float3(dxy, dz);
		float  len = length(raw);
		float3 dir = (len > 1e-4f) ? (raw / len) : float3(0.0f, 0.0f, 1.0f);
		float speed = ep.mean + ep.stddev * rand01(state);
		final_vel += dir * speed;
	} else { // SPAWN_DIST_GAUSSIAN
		float2 vxy = rand_normal2(state, emitter_id);
		float  vz  = rand_normal2(state, emitter_id).x;
		final_vel += float3(vxy.x, vxy.y, vz);
	}

	// Life: caller-supplied positive overrides; else random in [min_life, max_life].
	if (life <= 0.0f)
		life = ep.min_life + (ep.max_life - ep.min_life) * rand01(state);

	// Per-particle brightness
	color.rgb *= ep.brightness;

	float scale = pos_scale.w;
	if (scale <= 0.0f)
		scale = (ep.spawn_scale > 0.0f) ? ep.spawn_scale : 0.1f + 0.1f * rand01(state);

	float trail_phase = ep.trail_interval * rand01(state);

	Particle p;
	p.position = float4(pos_scale.xyz, scale);
	p.velocity = float4(final_vel, life);
	p.color = color;
	p.tex_coords = float4(0.0f);
	p.atlas_index = ep.atlas_index;
	p.max_life = life;
	p.blend = 0.0f;
	p.emitter_id = emitter_id;
	p.trail_accumulator = trail_phase;
	p.spawn_scale = scale;
	p._pad0 = 0.0f;
	p._pad1 = 0.0f;
	return p;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void compMain(uint3 group_id : SV_GroupID, uint3 group_thread_id : SV_GroupThreadID) {
	uint event_id = group_id.x;
	if (event_id >= params.spawn_event_count)
		return;

	SpawnEvent e = spawn_events[event_id];
	uint count = e.count;

	for (uint k = group_thread_id.x; k < count; k += 256u) {
		uint slot;
		if (!popFreeIndex(slot))
			return; // free list drained this dispatch; remaining particles for this event are dropped.

		Particle p = buildParticleFromEvent(e, event_id, k);
		particles_out[slot] = p;
		appendToRenderBuffer(p);
	}
}