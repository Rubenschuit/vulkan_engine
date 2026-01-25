#include "pch.hpp"
#include "systems/fireworks_system.hpp"
#include "utils/ve_random.hpp"
#include <glm/glm.hpp>
#include <random>

namespace ve {

FireworksSystem::FireworksSystem(
    VeDevice& device,
    std::shared_ptr<VeDescriptorPool> descriptor_pool,
    const vk::raii::DescriptorSetLayout& global_set_layout,
    const vk::raii::DescriptorSetLayout& texture_set_layout,
    vk::Format color_format,
    vk::SampleCountFlagBits sample_count,
    std::filesystem::path shader_path) {

    m_particle_system = std::make_unique<ParticleSystem>(
        device,
        descriptor_pool,
        global_set_layout,
        texture_set_layout,
        color_format,
        sample_count,
        m_config.max_particles, // Dedicated particle count for fireworks
        glm::vec3(0.0f), // origin
        shader_path,
		false
    );

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
		.info = {0, 1, TYPE_ROCKET, 0}
	};
    m_particle_system->emitParticles(event);
}

void FireworksSystem::recordComputeCommands(VeFrameInfo& frame_info) {
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

	// Update active rockets
    for (auto it = m_rockets.begin(); it != m_rockets.end(); ) {
        Rocket& r = *it;

		// apply physics
		glm::vec3 wind_velocity = glm::normalize(m_config.wind_direction) * m_config.wind_strength;
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

		// Trail emission should be framerate independent: emit every 4ms
		if (r.trail_timer <= 0.0f) {
			r.trail_timer = m_config.trail_interval;

            if (r.type == 0) {

                SpawnEvent smoke{
                    .position_scale = glm::vec4(r.pos, 5.0f),
                    .velocity_life = glm::vec4(r.vel * 0.02f, 7.0f),
                    .color = m_config.smoke_color,
                    .info = {0, 2, TYPE_SMOKE, 0}
                };
                m_particle_system->emitParticles(smoke);

                SpawnEvent spark{
                    .position_scale = glm::vec4(r.pos, 0.2f),
                    .velocity_life = glm::vec4(-r.vel * 0.0f, 0.34f),
                    .color = glm::vec4(1.0f, 0.8f, 0.1f, 1.0f),
                    .info = {0, 5, TYPE_SPARK, 0}
                };
                m_particle_system->emitParticles(spark);
            }
		}

        // Explosion check
        bool dead = (r.timer <= 0.0f);
        // Rocket specific explosion condition
        if (r.type == 0 && r.vel.z < -5.0f) dead = true;

        if (dead) {
            if (r.type == 0) {
				uint32_t streamer_count = static_cast<uint32_t>(Random::intRange(m_config.explosion_particle_count, m_config.explosion_particle_count * 2));

				// Each streamer is a single particle emitting a trail of particles spawned from the shader
                SpawnEvent streamers{
                    .position_scale = glm::vec4(r.pos, m_config.explosion_size),
                    .velocity_life = glm::vec4(0.0f, 0.0f, 0.0f, 2.5f),
                    .color = r.color,
                    .info = {0, streamer_count, TYPE_STREAMER, 0}
                };
                m_particle_system->emitParticles(streamers);
            }

            it = m_rockets.erase(it);
        } else {
            ++it;
        }
    }

    // Update the internal particle system
    m_particle_system->recordComputeCommands(frame_info);
}

void FireworksSystem::render(VeFrameInfo& frame_info) const {
    m_particle_system->render(frame_info);
}

}
