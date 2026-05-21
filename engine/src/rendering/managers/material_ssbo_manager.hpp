#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_buffer.hpp"
#include "resources/ve_material_properties.hpp"
#include "events/event_bus.hpp"

#include <unordered_map>
#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace ve {

class VeDevice;
class VeMaterial;
class BindlessTextureRegistry;
class EventBus;

struct MaterialGPU {
	glm::vec4 base_color_factor;
	glm::vec4 emissive_factor;       // xyz=emissive, w=pad
	glm::vec4 pbr_params;            // metallic, roughness, emissive_strength, transmission
	glm::vec4 specular_color_ior;
	uint32_t albedo_index;
	uint32_t normal_index;
	uint32_t metallic_roughness_index;
	uint32_t occlusion_index;
	uint32_t emissive_index;
	uint32_t material_flags;         // see MaterialFlag namespace in ve_config.hpp
	float    alpha_cutoff;
	uint32_t specular_index;
	uint32_t specular_color_index;
	float    specular_strength;
	uint32_t _pad0;
	uint32_t _pad1;
};
static_assert(sizeof(MaterialGPU) == 112, "MaterialGPU must be 112 bytes for SSBO alignment");

class VENGINE_API MaterialSSBOManager {
public:
	MaterialSSBOManager(VeDevice& device, BindlessTextureRegistry& texture_registry, EventBus& event_bus);
	~MaterialSSBOManager();

	MaterialSSBOManager(const MaterialSSBOManager&) = delete;
	MaterialSSBOManager& operator=(const MaterialSSBOManager&) = delete;

	// Returns the SSBO index for this material, allocating a slot on the
	// first call.
	uint32_t indexFor(VeMaterial* mat);

	void updateMaterial(uint32_t index, VeMaterial* mat);

	// Called by VeMaterial::doUnload to release this material's SSBO slot.
	void releaseSlot(VeMaterial* mat);

	void flushToDevice(const vk::raii::CommandBuffer&);

	bool isDirty() const { return m_any_dirty; }

	// Clear all cached material mappings. Intended for full scene tear-down.
	void reset();

	VeBuffer& getBuffer() { return *m_buffer; }

private:
	void writeMaterialGPU(uint32_t index, VeMaterial* mat);

	VeDevice& m_ve_device;
	BindlessTextureRegistry& m_texture_registry;
	EventBus& m_event_bus;
	static constexpr EventSubscriptionId NO_SUB = static_cast<EventSubscriptionId>(-1);
	EventSubscriptionId m_unload_sub = NO_SUB;

	std::unique_ptr<VeBuffer> m_buffer;          // device-local
	std::unique_ptr<VeBuffer> m_staging_buffer;  // host-visible, persistently mapped
	std::vector<uint8_t> m_dirty_slots;
	bool m_any_dirty = false;
	std::unordered_map<VeMaterial*, uint32_t> m_material_to_index;
	std::vector<uint32_t> m_free_list;
	uint32_t m_next_index = 0;
};

} // namespace ve
