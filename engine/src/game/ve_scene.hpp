#pragma once
#include "ve_export.hpp"
#include "ve_game_object.hpp"
#include <string>
#include <unordered_map>

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

namespace ve {

class VeDevice;

enum class AlphaMode : uint32_t {
	OPAQUE = 0,
	MASK = 1,
	BLEND = 2,
};

// Add to Material class in ve_model.hpp?
struct MaterialAlphaProps {
	AlphaMode alpha_mode = AlphaMode::OPAQUE;
	float alpha_cutoff = 0.5f;
	bool double_sided = false;
};

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
    virtual void update(float /*dt*/) {}

protected:
    VeDevice& m_device;
    std::string m_name;
    std::unordered_map<uint32_t, VeGameObject> m_game_objects;
	uint32_t m_num_lights;
	uint32_t m_num_shadow_casting_lights;

    friend class VeGameObject;
};

}