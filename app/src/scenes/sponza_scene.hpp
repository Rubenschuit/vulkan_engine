#pragma once
#include "game/ve_scene.hpp"
#include "core/ve_descriptors.hpp"
#include <memory>
#include <filesystem>

namespace ve {

class SponzaScene : public VeScene {
public:
    SponzaScene(VeDevice& device, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const std::filesystem::path& project_root);

    vk::raii::DescriptorSet& getDescriptorSet() override;
    Type getType() const override { return Type::PBR; }

    void setSunIntensity(float intensity);
    uint32_t getSponzaId() const { return m_sponza_id; }

private:
    void loadGameObjects(VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const std::filesystem::path& project_root);

    uint32_t m_sponza_id;
    uint32_t m_sun_id;
};

}

