/* This is a very ad hoc fireworks particle system, which consists of
 * launching "rockets" that explode into trail-emitting particles. Rockets also
 * emit a smoke and spark trail. It uses a compute shader to update particles
 * and a graphics pipeline to render them.
 */
#pragma once
#include "ve_export.hpp"
#include "rendering/particle_system.hpp"
#include "rendering/ve_frame_info.hpp"
#include <vector>
#include <memory>
#include <glm/glm.hpp>

namespace ve {

struct Rocket {
    glm::vec3 pos;
    glm::vec3 vel;
    glm::vec4 color;
    float timer;
    float trail_timer = 0.0f;
    bool exploded = false;
    int type = 0; // 0 = 'palm tree' firework
};

struct FireworksConfig {
    glm::vec3 launch_pos{0.0f, 0.0f, 0.0f};
    float launch_vel_min = 60.0f;
    float launch_vel_max = 110.0f;
    float launch_spread_xy = 15.0f;
    int launch_count = 1;

    glm::vec4 particle_color{1.0f};
    bool use_random_color = true;

    int explosion_particle_count = 60;
    float explosion_size = 0.8f;

    glm::vec3 wind_direction{1.0f, 1.0f, 0.0f};
    float wind_strength = 3.0f;

    float gravity = 15.0f;

    int max_particles = 100000;

	float trail_interval = 0.012f;
	glm::vec4 smoke_color = glm::vec4(0.4f, 0.4f, 0.4f, 0.5f);
};

class VENGINE_API FireworksSystem {
public:
    FireworksSystem(
        VeDevice& device,
        std::shared_ptr<VeDescriptorPool> descriptor_pool,
        const vk::raii::DescriptorSetLayout& global_set_layout,
        const vk::raii::DescriptorSetLayout& texture_set_layout,
        vk::Format color_format,
        vk::SampleCountFlagBits sample_count,
        std::filesystem::path shader_path);

    void recordComputeCommands(VeFrameInfo& frame_info);
    void render(VeFrameInfo& frame_info) const;
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
		m_particle_system->recreatePipeline(color_format, sample_count);
	}

	void launchRocket(); // use m_config to launch a rocket
    void launchRocket(glm::vec3 pos, glm::vec3 vel, glm::vec4 color);
    void setParticleCapacity(uint32_t capacity);

    FireworksConfig& getConfig() { return m_config; }

	void setEnabled(bool enabled) { m_enabled = enabled; m_particle_system->setEnabled(enabled); }
	bool isEnabled() const { return m_enabled; }

private:
	// Longest particle lifetime in the fireworks system (smoke = 7s).
	// After all rockets die, wait this long before going fully idle.
	static constexpr float COOLDOWN_TIME = 8.0f;

	bool m_enabled{true};
	float m_idle_timer{0.0f}; // counts down after last rocket dies
    std::unique_ptr<ParticleSystem> m_particle_system;
    std::vector<Rocket> m_rockets;
    FireworksConfig m_config;
    uint32_t m_pending_capacity{0};
};

}
