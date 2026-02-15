#pragma once
#include "ve_export.hpp"
#include "vulkan/ve_device.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_material_properties.hpp"
#include <string>

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

    Registry& getRegistry() { return m_registry; }
    const Registry& getRegistry() const { return m_registry; }

    virtual vk::raii::DescriptorSet& getDescriptorSet() = 0;
    virtual Type getType() const = 0;
    virtual void update(float dt);

    // Sun light intensity. Scenes without a sun return 0.
    virtual float getSunIntensity() const { return 0.0f; }
    virtual void setSunIntensity(float intensity) { (void)intensity; }
    // Entity of the scene sun (for UI sync). Null if no sun.
    virtual Entity getSun() const { return Entity::null(); }

protected:
    VeDevice& m_device;
    std::string m_name;
    Registry m_registry;
	uint32_t m_num_lights;
	uint32_t m_num_shadow_casting_lights;
};

}