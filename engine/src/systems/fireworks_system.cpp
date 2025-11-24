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
        100000, // Dedicated particle count for fireworks
        glm::vec3(0.0f), // origin
        shader_path,
		false
    );

    m_particle_system->setLifeRange(0.5f, 3.0f);
    // Disable auto-respawn for fireworks, they should be spawned by events
    m_particle_system->setShouldRespawn(false);

    // initialize config max_particles
    m_config.max_particles = 100000;
}

void FireworksSystem::setParticleCapacity(uint32_t capacity) {
    m_pending_capacity = capacity;
}

void FireworksSystem::launchRocket(glm::vec3 pos, glm::vec3 vel, glm::vec4 color) {
    Rocket r{
		.pos = pos,
		.vel = vel,
		.color = color,
		.timer = Random::floatRange(4.0f, 7.0f), // explode time
		.exploded = false
	};
    m_rockets.push_back(r);

    // Emit the rocket particle (not used visually atm)
    SpawnEvent event{
		.position_scale = glm::vec4(pos, 0.0f),
		.velocity_life = glm::vec4(vel, 2.0f),
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

    for (auto it = m_rockets.begin(); it != m_rockets.end(); ) {
        Rocket& r = *it;

        // Physics
		glm::vec3 wind = glm::length(m_config.wind_direction) > 0.0f ? glm::normalize(m_config.wind_direction) * m_config.wind_strength : glm::vec3(0.0f);
        r.vel.z -= m_config.gravity * dt;
        r.pos += r.vel * dt;
		r.pos += wind * dt;
		float spiral_factor = glm::clamp(r.vel.z, 0.0f, 10.0f);
		float spiral_speed = glm::clamp(r.vel.z, 0.0f, 5.0f);
		r.pos.x += glm::cos(frame_info.total_time * spiral_speed) * spiral_factor * dt;
		r.pos.y += glm::sin(frame_info.total_time * spiral_speed) * spiral_factor * dt;
        r.timer -= dt;

        // Trail emission
		float smoke_variance = 0.1f;
        SpawnEvent smoke{
			.position_scale = glm::vec4(r.pos, 5.0f),
			.velocity_life = glm::vec4(0.3f * wind + r.vel * 0.02f, 7.0f),
			.color = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f),
			.info = {0, 2, TYPE_SMOKE, *reinterpret_cast<uint32_t*>(&smoke_variance)}
		};

        m_particle_system->emitParticles(smoke);

        SpawnEvent spark{
			.position_scale = glm::vec4(r.pos, 0.2f),
			.velocity_life = glm::vec4(-r.vel * 0.4f, 0.3f),
			.color = glm::vec4(1.0f, 0.8f, 0.1f, 1.0f),
			.info = {0, 5, TYPE_SPARK, 0}
		};
        m_particle_system->emitParticles(spark);

        // Explode condition: Timer or falling too fast
        if (r.timer <= 0.0f || r.vel.z < -5.0f) {
            // Explode
            SpawnEvent explode{
				.position_scale = glm::vec4(r.pos, m_config.explosion_size),
				.velocity_life = glm::vec4(r.vel * 1.5f, 2.2f),
				.color = r.color,
				.info = {0, static_cast<uint32_t>(m_config.explosion_particle_count), TYPE_EXPLOSION, 0}
			};
            m_particle_system->emitParticles(explode);

            // Smoke from explosion
            SpawnEvent smoke_explode = explode;
            smoke_explode.position_scale.w = 10.0f;
			smoke_explode.velocity_life = glm::vec4(r.vel * 1.5f + wind * 0.3f, 7.0f);
            smoke_explode.color = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f);
            smoke_explode.info.z = TYPE_EXPLOSION_SMOKE;
			smoke_explode.info.y = static_cast<uint32_t>(m_config.explosion_particle_count);
            m_particle_system->emitParticles(smoke_explode);

            it = m_rockets.erase(it);
        } else {
            ++it;
        }
    }

    // Update the internal particle system
    m_particle_system->update(frame_info);
}

void FireworksSystem::render(VeFrameInfo& frame_info) const {
    m_particle_system->render(frame_info);
}

}
