#pragma once
#include "../asset_paths.hpp"
#include "scene/ve_scene.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_resource_manager.hpp"
#include <memory>
#include <filesystem>

namespace ve {

class VeModel;

class SponzaScene : public VeScene {
public:
    // variant: selects KTX2 quality preset (sponza or sponza_low (ETC1S))
    SponzaScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, const char* variant = "sponza");

    vk::raii::DescriptorSet& getDescriptorSet() override;
    vk::raii::DescriptorSet& getDescriptorSet(const VeGameObject* obj) override;
    MaterialAlphaProps getMaterialAlphaProps(const VeGameObject* obj) const override;
    Type getType() const override { return Type::PBR; }

    void setSunIntensity(float intensity);

private:
    void loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, const char* variant);

    std::unique_ptr<VeModel> m_sponza_model;
    uint32_t m_sun_id;
};

}

