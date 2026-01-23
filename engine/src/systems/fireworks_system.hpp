#pragma once
#include "ve_export.hpp"
#include "systems/particle_system.hpp"
#include "game/ve_frame_info.hpp"
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

    int explosion_particle_count = 100;
    float explosion_size = 0.8f;

    glm::vec3 wind_direction{1.0f, 1.0f, 0.0f};
    float wind_strength = 3.0f;

    float gravity = 15.0f;

    int max_particles = 1000000;

	float trail_interval = 0.004f; // 4ms
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

private:
    std::unique_ptr<ParticleSystem> m_particle_system;
    std::vector<Rocket> m_rockets;
    FireworksConfig m_config;
    uint32_t m_pending_capacity{0};
};

}
