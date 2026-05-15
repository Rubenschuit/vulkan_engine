#include "pch.hpp"
#include "rendering/fireworks_system.hpp"
#include "utils/ve_random.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include <glm/glm.hpp>
#include <random>

namespace ve {

FireworksSystem::FireworksSystem(const FireworksSystemCreateInfo& info) {

	info.event_bus.subscribe<PipelineRecreateEvent>([this](const PipelineRecreateEvent& e) {
		recreatePipeline(e.offscreen_format, e.sample_count);
	});

    m_particle_system = std::make_unique<ParticleSystem>(ParticleSystemCreateInfo{
        .device = info.device,
        .descriptor_pool = info.descriptor_pool,
        .global_set_layout = info.global_set_layout,
        .particle_texture = info.particle_texture,
        .fire_texture = info.fire_texture,
        .smoke_texture = info.smoke_texture,
        .color_format = info.color_format,
        .sample_count = info.sample_count,
        .particle_count = static_cast<uint32_t>(m_config.max_particles),
        .origin = glm::vec3(0.0f),
        .shader_path = info.shader_path,
        .start_active = false,
    });

    m_particle_system->setLifeRange(0.5f, 3.0f);
    // Disable auto-respawn for fireworks, they should be spawned by events
    m_particle_system->setShouldRespawn(false);
}

void FireworksSystem::setParticleCapacity(uint32_t capacity) {
    m_pending_capacity = capacity;
}

void FireworksSystem::launchRocket() {
	for (int i = 0; i < m_config.launch_count; i++) {
		glm::vec3 pos = m_config.launch_pos;
		//random upwards velocity
		float speed = Random::floatRange(m_config.launch_vel_min, m_config.launch_vel_max);
		float spread_x = Random::floatRange(-m_config.launch_spread_xy, m_config.launch_spread_xy);
		float spread_y = Random::floatRange(-m_config.launch_spread_xy, m_config.launch_spread_xy);
		glm::vec3 vel = glm::vec3(spread_x, spread_y, speed);
		glm::vec4 color = m_config.use_random_color ? Random::colorHSV() : m_config.particle_color;
		launchRocket(pos, vel, color);
	}
}

void FireworksSystem::launchRocket(glm::vec3 pos, glm::vec3 vel, glm::vec4 color) {
    Rocket r{
		.pos = pos,
		.vel = vel,
		.color = color,
		.timer = Random::floatRange(2.0f, 7.0f), // explode time
		.trail_timer = 0.0f,
		.exploded = false,
        .type = 0
	};
    m_rockets.push_back(r);

    // Emit a singular rocket particle (invisible until it explodes, then it simulates a flash)
    SpawnEvent event{
		.position_scale = glm::vec4(pos, 0.0f),
		.velocity_life = glm::vec4(vel, r.timer),
		.color = color,
		.info = {0, 1, static_cast<uint32_t>(ParticleType::ROCKET), 0}
	};
    m_particle_system->emitParticles(event);
}

void FireworksSystem::recordComputeCommands(VeFrameInfo& frame_info) {
	if (!m_enabled) return;
    // Handle pending capacity change at the start of update (safe point)
    if (m_pending_capacity > 0) {
        if (m_particle_system) {
             m_particle_system->setParticleCount(m_pending_capacity, true); // true = reset buffers
             m_config.max_particles = static_cast<int>(m_pending_capacity);
        }
        m_pending_capacity = 0;
    }

    float dt = frame_info.frame_time;

	// Set parameters from ui
	glm::vec3 dir = glm::length(m_config.wind_direction) > 0.001f
		? glm::normalize(m_config.wind_direction)
		: glm::vec3(1.0f, 0.0f, 0.0f);
	m_particle_system->setWind(glm::vec4(dir, m_config.wind_strength));
	m_particle_system->setGravity(m_config.gravity);
	m_particle_system->setTrailInterval(m_config.trail_interval);

	glm::vec3 wind_velocity = dir * m_config.wind_strength;

	// Update active rockets (swap-and-pop removal for O(1) erase)
	for (size_t i = 0; i < m_rockets.size(); ) {
		Rocket& r = m_rockets[i];

		// apply physics
		r.pos += wind_velocity * dt;
		r.vel.z -= m_config.gravity * dt;
		r.pos += r.vel * dt;

		if (r.type == 0) {
			float spiral_factor = glm::clamp(r.vel.z, 0.0f, 10.0f);
			float spiral_speed = glm::clamp(r.vel.z, 0.0f, 5.0f);
			float rocket_seed = r.color.r * 1000.0f; // desync the spirals of different rockets
			r.pos.x += glm::cos(frame_info.total_time * spiral_speed + rocket_seed) * spiral_factor * dt;
			r.pos.y += glm::sin(frame_info.total_time * spiral_speed + rocket_seed) * spiral_factor * dt;
		}
		r.timer -= dt;
		r.trail_timer -= dt;

		// Trail emission should be framerate independent: emit every trail_interval
		if (r.trail_timer <= 0.0f) {
			r.trail_timer = m_config.trail_interval;

			if (r.type == 0) {
				SpawnEvent smoke{
					.position_scale = glm::vec4(r.pos, 5.0f),
					.velocity_life = glm::vec4(r.vel * 0.02f, 7.0f),
					.color = m_config.smoke_color,
					.info = {0, 2, static_cast<uint32_t>(ParticleType::SMOKE), 0}
				};
				m_particle_system->emitParticles(smoke);

				SpawnEvent spark{
					.position_scale = glm::vec4(r.pos, 0.2f),
					.velocity_life = glm::vec4(-r.vel * 0.0f, 0.34f),
					.color = glm::vec4(1.0f, 0.8f, 0.1f, 1.0f),
					.info = {0, 5, static_cast<uint32_t>(ParticleType::SPARK), 0}
				};
				m_particle_system->emitParticles(spark);
			}
		}

		// Explosion check
		bool dead = (r.timer <= 0.0f);
		if (r.type == 0 && r.vel.z < -5.0f) dead = true;

		if (dead) {
			if (r.type == 0) {
				uint32_t streamer_count = static_cast<uint32_t>(Random::intRange(m_config.explosion_particle_count, m_config.explosion_particle_count * 2));

				SpawnEvent streamers{
					.position_scale = glm::vec4(r.pos, m_config.explosion_size),
					.velocity_life = glm::vec4(0.0f, 0.0f, 0.0f, 2.5f),
					.color = r.color,
					.info = {0, streamer_count, static_cast<uint32_t>(ParticleType::STREAMER), 0}
				};
				m_particle_system->emitParticles(streamers);
			}

			// Swap-and-pop: O(1) removal, order doesn't matter for rockets
			m_rockets[i] = m_rockets.back();
			m_rockets.pop_back();
		} else {
			++i;
		}
	}

    // Track idle state: skip compute entirely when no rockets and all particles have died
    if (!m_rockets.empty()) {
        m_idle_timer = COOLDOWN_TIME;
    } else {
        m_idle_timer -= dt;
    }

    // Only dispatch compute when there's active work (rockets flying or particles still alive)
    if (m_idle_timer > 0.0f) {
        m_particle_system->recordComputeCommands(frame_info);
    }
}

void FireworksSystem::render(VeFrameInfo& frame_info) const {
	if (!m_enabled || m_idle_timer <= 0.0f)
		return;
    m_particle_system->render(frame_info);
}

}
