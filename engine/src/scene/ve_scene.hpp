#pragma once
#include "ve_export.hpp"
#include "vulkan/ve_device.hpp"
#include "scene/ve_game_object.hpp"
#include "resources/ve_material_properties.hpp"
#include <string>
#include <unordered_map>

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

namespace ve {

class VENGINE_API VeScene {
public:
    enum class Type { SIMPLE, PBR };

    VeScene(VeDevice& device, const std::string& name);
    virtual ~VeScene();

    VeScene(const VeScene&) = delete;
    VeScene& operator=(const VeScene&) = delete;

    std::unordered_map<uint32_t, VeGameObject>& getGameObjects();
    const std::unordered_map<uint32_t, VeGameObject>& getGameObjects() const;

    virtual vk::raii::DescriptorSet& getDescriptorSet() = 0;
    // Per-object material descriptor set (for multi-material models). Returns scene default when obj is null or not applicable.
    virtual vk::raii::DescriptorSet& getDescriptorSet(const VeGameObject* obj) { (void)obj; return getDescriptorSet(); }
    // Per-object material alpha props (alphaMode, alphaCutoff, doubleSided from glTF). Returns default when obj is null.
    virtual MaterialAlphaProps getMaterialAlphaProps(const VeGameObject* obj) const { (void)obj; return {}; }
    virtual Type getType() const = 0;
    virtual void update(float dt);

    // Sun light intensity. Scenes without a sun return 0.
    virtual float getSunIntensity() const { return 0.0f; }
    // Set sun light intensity. Does nothing in scenes without a sun.
    virtual void setSunIntensity(float intensity) { (void)intensity; }
    // Game object id of the scene sun (for UI sync). 0 if no sun.
    virtual uint32_t getSunId() const { return 0; }

protected:
    VeDevice& m_device;
    std::string m_name;
    std::unordered_map<uint32_t, VeGameObject> m_game_objects;
	uint32_t m_num_lights;
	uint32_t m_num_shadow_casting_lights;

    friend class VeGameObject;
};

}