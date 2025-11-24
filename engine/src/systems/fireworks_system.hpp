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
    bool exploded = false;
};

struct FireworksConfig {
    glm::vec3 launch_pos{0.0f, -150.0f, 0.0f};
    float launch_vel_min = 60.0f;
    float launch_vel_max = 110.0f;
    float launch_spread_xy = 15.0f;
    int launch_count = 1;

    glm::vec4 particle_color{1.0f};
    bool use_random_color = true;

    int explosion_particle_count = 5000;
    float explosion_size = 0.8f;

    glm::vec3 wind_direction{1.0f, 0.0f, 0.0f};
    float wind_strength = 6.0f;

    float gravity = 20.0f;

    int max_particles = 100000;
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

    void update(VeFrameInfo& frame_info);
    void render(VeFrameInfo& frame_info) const;

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
