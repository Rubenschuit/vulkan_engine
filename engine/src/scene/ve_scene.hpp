#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_device.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_material_properties.hpp"
#include "resources/ve_resource_manager.hpp"
#include <string>
#include <filesystem>
#include <memory>
#include <vector>

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

namespace ve {

class VeModel;
class VeDescriptorPool;
class VeDescriptorSetLayout;

struct VENGINE_API SceneContext {
    VeDevice& device;
    VeResourceManager& resource_manager;
    VeDescriptorPool& pool;
    VeDescriptorSetLayout& material_layout;
};

class VENGINE_API VeScene {
public:
    VeScene(const SceneContext& ctx, const std::string& name);
    virtual ~VeScene();

    VeScene(const VeScene&) = delete;
    VeScene& operator=(const VeScene&) = delete;

    Registry& getRegistry() { return m_registry; }
    const Registry& getRegistry() const { return m_registry; }

    const std::string& getName() const { return m_name; }

    // Load a GLTF model and add it to the scene.
    void addModel(const std::filesystem::path& gltf_path);

    virtual void update(float dt);

    // Per-scene ambient light defaults (color RGB, intensity in w)
    virtual glm::vec4 getDefaultAmbient() const { return DEFAULT_AMBIENT_LIGHT_COLOR; }

    // Particle pool capacity required by this scene. 0 = use the engine
    // default.
    virtual uint32_t getParticleCapacity() const { return 0; }

protected:
    VeDevice& m_device;
    VeResourceManager& m_resource_manager;
    VeDescriptorPool& m_pool;
    VeDescriptorSetLayout& m_material_layout;
    std::string m_name;
    Registry m_registry;
	uint32_t m_num_lights;
	uint32_t m_num_shadow_casting_lights;

    std::vector<std::unique_ptr<VeModel>> m_models;
};

}