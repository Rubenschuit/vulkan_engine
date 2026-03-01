#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_buffer.hpp"
#include "resources/ve_material_properties.hpp"

#include <unordered_map>
#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace ve {

class VeDevice;
class VeMaterial;
class BindlessTextureRegistry;

struct MaterialGPU {
	glm::vec4 base_color_factor;
	glm::vec4 emissive_pad;    // xyz=emissive, w=pad
	glm::vec4 params;          // metallic, roughness, emissive_strength, transmission
	glm::vec4 extra;           // specular xyz, ior w
	uint32_t albedo_index;
	uint32_t normal_index;
	uint32_t metallic_roughness_index;
	uint32_t occlusion_index;
	uint32_t emissive_index;
	uint32_t material_flags;   // bits 0-1: alpha_mode, bit 2: double_sided, bit 3: flip_tex_v, bit 4: spec_gloss, bit 5: has_texture
	float    alpha_cutoff;
	uint32_t _pad;
};
static_assert(sizeof(MaterialGPU) == 96, "MaterialGPU must be 96 bytes for SSBO alignment");

class VENGINE_API MaterialSSBOManager {
public:
	MaterialSSBOManager(VeDevice& device, BindlessTextureRegistry& texture_registry);
	~MaterialSSBOManager();

	MaterialSSBOManager(const MaterialSSBOManager&) = delete;
	MaterialSSBOManager& operator=(const MaterialSSBOManager&) = delete;

	uint32_t registerMaterial(VeMaterial* mat);
	void updateMaterial(uint32_t index, VeMaterial* mat);
	void unregisterMaterial(VeMaterial* mat);

	void flushToDevice(const vk::raii::CommandBuffer&);

	// Clear all cached material mappings (call when scene changes)
	void reset();

	VeBuffer& getBuffer() { return *m_buffer; }

private:
	void writeMaterialGPU(uint32_t index, VeMaterial* mat);

	VeDevice& m_ve_device;
	BindlessTextureRegistry& m_texture_registry;

	std::unique_ptr<VeBuffer> m_buffer;          // device-local
	std::unique_ptr<VeBuffer> m_staging_buffer;  // host-visible, persistently mapped
	bool m_dirty = false;
	std::unordered_map<VeMaterial*, uint32_t> m_material_to_index;
	std::vector<uint32_t> m_free_list;
	uint32_t m_next_index = 0;
};

} // namespace ve
