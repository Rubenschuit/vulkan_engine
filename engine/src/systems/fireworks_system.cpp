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
    std::filesystem::path shader_path) {

    m_particle_system = std::make_unique<ParticleSystem>(
        device,
        descriptor_pool,
        global_set_layout,
        texture_set_layout,
        color_format,
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
		glm::vec4 color = m_config.use_random_color ? Random::color() : m_config.particle_color;
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
        .generation = 0
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

void FireworksSystem::update(VeFrameInfo& frame_info) {
    // Handle pending capacity change at the start of update (safe point)
    if (m_pending_capacity > 0) {
        if (m_particle_system) {
             m_particle_system->setParticleCount(m_pending_capacity, true); // true = reset buffers
             m_config.max_particles = static_cast<int>(m_pending_capacity);
        }
        m_pending_capacity = 0;
    }

    float dt = frame_info.frame_time;

	// Set wind direction/strength and gravity from ui
	glm::vec3 dir = glm::length(m_config.wind_direction) > 0.001f
		? glm::normalize(m_config.wind_direction)
		: glm::vec3(1.0f, 0.0f, 0.0f);
	m_particle_system->setWind(glm::vec4(dir, m_config.wind_strength));
	m_particle_system->setGravity(m_config.gravity);

    std::vector<Rocket> new_rockets; // Store new streamers here

    for (auto it = m_rockets.begin(); it != m_rockets.end(); ) {
        Rocket& r = *it;

		// apply physics
		glm::vec3 wind_velocity = glm::normalize(m_config.wind_direction) * m_config.wind_strength;
		r.pos += wind_velocity * dt;
        r.vel.z -= m_config.gravity * dt;
        r.pos += r.vel * dt;

        // GENERATION 0: The Main Rocket (Spirals + Smoke)
        if (r.generation == 0) {
            float spiral_factor = glm::clamp(r.vel.z, 0.0f, 10.0f);
            float spiral_speed = glm::clamp(r.vel.z, 0.0f, 5.0f);
            float rocket_seed = r.color.r * 1000.0f; // desync the spirals of different rockets
            r.pos.x += glm::cos(frame_info.total_time * spiral_speed + rocket_seed) * spiral_factor * dt;
            r.pos.y += glm::sin(frame_info.total_time * spiral_speed + rocket_seed) * spiral_factor * dt;
        }
        // GENERATION 1: The Palm Streamers (High Drag + Gold Trails)
        else if (r.generation == 1) {
            // Apply drag to create the "hanging" effect of a willow firework
            r.vel *= (1.0f - 1.5f * dt);
        }

        r.timer -= dt;
		r.trail_timer -= dt;

		// Trail emission should be framerate independent: emit every 4ms
		if (r.trail_timer <= m_config.trail_interval) {
			r.trail_timer = m_config.trail_interval;
			float smoke_variance = 0.1f;

            if (r.generation == 0) {

                SpawnEvent smoke{
                    .position_scale = glm::vec4(r.pos, 5.0f),
                    .velocity_life = glm::vec4(r.vel * 0.02f, 7.0f),
                    .color = m_config.smoke_color,
                    .info = {0, 2, TYPE_SMOKE, *reinterpret_cast<uint32_t*>(&smoke_variance)}
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

        // DEATH / EXPLOSION CHECK
        bool dead = (r.timer <= 0.0f);
        // Rocket specific fail condition
        if (r.generation == 0 && r.vel.z < -5.0f) dead = true;

        if (dead) {
            if (r.generation == 0) {

                SpawnEvent streamers{
                    .position_scale = glm::vec4(r.pos, 0.3f),
                    .velocity_life = glm::vec4(0.0f, 0.0f, 0.0f, 2.5f),
                    .color = r.color,
                    .info = {0, static_cast<uint32_t>(m_config.explosion_particle_count), 6, 0}
                };
                m_particle_system->emitParticles(streamers);
            }

            it = m_rockets.erase(it);
        } else {
            ++it;
        }
    }

    // Add newly spawned streamers to the main list
    m_rockets.insert(m_rockets.end(), new_rockets.begin(), new_rockets.end());

    // Update the internal particle system
    m_particle_system->update(frame_info);
}

void FireworksSystem::render(VeFrameInfo& frame_info) const {
    m_particle_system->render(frame_info);
}

}
