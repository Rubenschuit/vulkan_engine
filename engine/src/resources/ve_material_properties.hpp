#pragma once
#include "ve_export.hpp"
#include <glm/glm.hpp>

namespace ve {

enum class AlphaMode : uint32_t {
	ALPHA_OPAQUE = 0, // OPAQUE is defined by some windows headers...
	MASK = 1,
	BLEND = 2,
};

struct MaterialAlphaProps {
	AlphaMode alpha_mode = AlphaMode::ALPHA_OPAQUE;
	float alpha_cutoff = 0.5f;
	bool double_sided = false;
	bool use_spec_gloss_texture = false;  // MR texture is specularGlossinessTexture (RGB=specular, A=glossiness)
};

// PBR data from glTF
struct MaterialFactors {
	glm::vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
	glm::vec3 emissive_factor{0.0f, 0.0f, 0.0f};
	float metallic_factor = 1.0f;
	float roughness_factor = 1.0f;
	float emissive_strength = 0.00058f;  // Engine default: attenuates emissive to prevent over-bright surfaces when KHR_materials_emissive_strength is absent
	float transmission_factor = 0.0f;
	float ior = 1.5f;  // KHR_materials_ior: index of refraction (default 1.5 gives F0 ≈ 0.04)
	glm::vec3 specular_factor{0.04f, 0.04f, 0.04f};  // Dielectric F0: from KHR_materials_pbrSpecularGlossiness.specularFactor or derived from ior.
};

// Must match shader's MaterialConstants struct (4 × float4 = 64 bytes).
static constexpr size_t MATERIAL_UBO_SIZE = 64;

// Write MaterialFactors to a float[16] array matching shader MaterialConstants layout:
//   float4 base_color_factor
//   float4 emissive_pad       (xyz = emissive_factor, w = pad)
//   float4 params             (metallic, roughness, emissive_strength, transmission)
//   float4 extra              (specular_factor.xyz, ior)
inline void writeMaterialUBO(float* dst, const MaterialFactors& f) {
	dst[0]  = f.base_color_factor.x;
	dst[1]  = f.base_color_factor.y;
	dst[2]  = f.base_color_factor.z;
	dst[3]  = f.base_color_factor.w;
	dst[4]  = f.emissive_factor.x;
	dst[5]  = f.emissive_factor.y;
	dst[6]  = f.emissive_factor.z;
	dst[7]  = 0.0f;
	dst[8]  = f.metallic_factor;
	dst[9]  = f.roughness_factor;
	dst[10] = f.emissive_strength;
	dst[11] = f.transmission_factor;
	dst[12] = f.specular_factor.x;
	dst[13] = f.specular_factor.y;
	dst[14] = f.specular_factor.z;
	dst[15] = f.ior;
}

} // namespace ve
