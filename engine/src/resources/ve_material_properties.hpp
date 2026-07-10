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
	bool use_spec_gloss = false;  // KHR_materials_pbrSpecularGlossiness: MR slot is specular(RGB)+glossiness(A), texture or unit default
	bool unlit = false;                   // KHR_materials_unlit: render base color, no lighting
};

// KHR_texture_transform: per-texture-slot UV offset/scale/rotation. Identity by
// default so materials without the extension are a no-op in the shader.
struct UvTransform {
	glm::vec2 offset{0.0f, 0.0f};
	glm::vec2 scale{1.0f, 1.0f};
	float rotation{0.0f};  // radians
};

struct MaterialUvTransforms {
	UvTransform albedo;
	UvTransform metallic_roughness;
	UvTransform normal;
	UvTransform occlusion;
	UvTransform emissive;
};

// PBR data from glTF
struct MaterialFactors {
	glm::vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
	glm::vec3 emissive_factor{0.0f, 0.0f, 0.0f};
	float metallic_factor = 1.0f;
	float roughness_factor = 1.0f;
	float occlusion_strength = 1.0f;  // glTF occlusionTexture.strength: ao = lerp(1, sample.r, strength)
	float emissive_strength = 0.00058f;  // Engine default: attenuates emissive to prevent over-bright surfaces when KHR_materials_emissive_strength is absent
	float transmission_factor = 0.0f;
	float ior = 1.5f;  // KHR_materials_ior: index of refraction (default 1.5 gives F0 ≈ 0.04)
	glm::vec3 specular_factor{1.0f, 1.0f, 1.0f};  // KHR_materials_specular.specularColorFactor (also raw F0 for KHR_materials_pbrSpecularGlossiness)
	float specular_strength = 1.0f;  // KHR_materials_specular.specularFactor
	glm::vec3 emissive_light_color{1.0f, 1.0f, 1.0f};
	float emissive_light_lum = -1.0f;
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
