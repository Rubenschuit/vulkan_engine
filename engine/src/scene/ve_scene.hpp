#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_device.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_material_properties.hpp"
#include "resources/ve_resource_manager.hpp"
#include <string>

namespace ve {

class EventBus;
struct AddModelRequestedEvent;

struct VENGINE_API SceneContext {
    VeDevice& device;
    VeResourceManager& resource_manager;
    EventBus& event_bus;
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

    virtual void update(float dt);

    // Per-scene ambient light defaults (color RGB, intensity in w)
    virtual glm::vec4 getDefaultAmbient() const { return DEFAULT_AMBIENT_LIGHT_COLOR; }

    // Particle pool capacity required by this scene. 0 = use the engine
    // default.
    virtual uint32_t getParticleCapacity() const { return 0; }

protected:
    // Async load a gltf and instantiate it into this scene's registry.
    void placeModel(const AddModelRequestedEvent& request);

    VeDevice& m_device;
    VeResourceManager& m_resource_manager;
    EventBus& m_event_bus;
    std::string m_name;
    Registry m_registry;
	uint32_t m_num_lights;
	uint32_t m_num_shadow_casting_lights;
};

}