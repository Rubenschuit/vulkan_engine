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

private:
    std::unique_ptr<ParticleSystem> m_particle_system;
    std::vector<Rocket> m_rockets;

    float m_gravity = 20.0f;
};

}
