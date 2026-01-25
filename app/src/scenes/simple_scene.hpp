#pragma once
#include "game/ve_scene.hpp"
#include "core/ve_texture.hpp"
#include "core/ve_descriptors.hpp"
#include <memory>
#include <filesystem>

namespace ve {

class SimpleScene : public VeScene {
public:
    SimpleScene(VeDevice& device, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const std::filesystem::path& project_root);

    vk::raii::DescriptorSet& getDescriptorSet() override { return m_texture_descriptor_set; }
    Type getType() const override { return Type::SIMPLE; }

private:
    void loadTextures(const std::filesystem::path& project_root);
    void createDescriptorSet(VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout);
    void loadGameObjects(const std::filesystem::path& project_root);

    std::unique_ptr<VeTexture> m_glow_texture;
    std::unique_ptr<VeTexture> m_fire_texture;
    std::unique_ptr<VeTexture> m_smoke_texture;
    vk::raii::DescriptorSet m_texture_descriptor_set{nullptr};
};

}

