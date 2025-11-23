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
}

void FireworksSystem::launchRocket(glm::vec3 pos, glm::vec3 vel, glm::vec4 color) {
    Rocket r;
    r.pos = pos;
    r.vel = vel;
    r.color = color;
    r.timer = Random::floatRange(4.0f, 7.0f); // explode time
    m_rockets.push_back(r);

    // Emit the rocket particle
    SpawnEvent e{};
    e.info.y = 1; // count
    e.info.z = TYPE_ROCKET;
    e.position_scale = glm::vec4(pos, 1.0f); // scale
    e.info.w = 0;

    e.velocity_life = glm::vec4(vel, 2.0f); // life > timer
    e.color = color;
    m_particle_system->emitParticles(e);
}

void FireworksSystem::update(VeFrameInfo& frame_info) {
    float dt = frame_info.frame_time;

    for (auto it = m_rockets.begin(); it != m_rockets.end(); ) {
        Rocket& r = *it;

        // Physics
        r.vel.z -= m_gravity * dt;
        r.pos += r.vel * dt;

		r.pos.x += glm::cos(frame_info.total_time * 5.0f) * 0.05f;
		r.pos.y += glm::sin(frame_info.total_time * 5.0f) * 0.05f;
        r.timer -= dt;

        // Trail emission
        SpawnEvent smoke{};
        smoke.info.y = 2; // count
        smoke.info.z = TYPE_SMOKE;
        smoke.position_scale = glm::vec4(r.pos, 4.0f);
        float smoke_variance = 0.2f;
        // bit_cast float to uint
        smoke.info.w = *reinterpret_cast<uint32_t*>(&smoke_variance);

		const float wind_speed = 5.0f;
        smoke.velocity_life = glm::vec4(wind_speed, 0.0f, 0.0f, 6.0f);
        smoke.color = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f);
        m_particle_system->emitParticles(smoke);

        SpawnEvent spark{};
        spark.info.y = 5; // count
        spark.info.z = TYPE_SPARK;
        spark.position_scale = glm::vec4(r.pos, 0.3f);
        float spark_variance = 0.1f;
        spark.info.w = *reinterpret_cast<uint32_t*>(&spark_variance);

        spark.velocity_life = glm::vec4(-r.vel * 0.4f, 0.3f); // backward vel
        spark.color = glm::vec4(1.0f, 0.8f, 0.1f, 1.0f);
        m_particle_system->emitParticles(spark);

        // Explode condition: Timer or falling too fast
        if (r.timer <= 0.0f || r.vel.z < -5.0f) {
            // Explode
            SpawnEvent burst{};
            burst.info.y = 5000; // count
            burst.info.z = TYPE_EXPLOSION; // Type 4 (Explosion)
            burst.position_scale = glm::vec4(r.pos, 0.8f); // scale

            float burst_variance = 0.0f; // Start from a single point
            burst.info.w = *reinterpret_cast<uint32_t*>(&burst_variance);

            burst.velocity_life = glm::vec4(r.vel * 1.5f, 2.2f); // inherit small amount of vel
            burst.color = r.color;
            m_particle_system->emitParticles(burst);

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
