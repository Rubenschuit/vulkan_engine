#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"

#include <glm/glm.hpp>
#include <cstdint>

namespace ve {

enum class CullingBackendMode : uint8_t {
	CPU,
	GPU,
	MESHLET,
};

struct VENGINE_API RenderSettings {
	// graphics
	float fov = 75.0f;
	ShadowMode shadow_mode = ShadowMode::PCF;
	ShadowResolutionPreset shadow_resolution_preset = ShadowResolutionPreset::MEDIUM;
	float pcss_light_size = 0.04f;
	int pcf_samples = 8;
	int pcss_filter_samples = 16;
	int csm_blend_mode = 2; // 0=off, 1=linear, 2=dithered
	float shadow_bias = ve::SHADOW_BIAS;
	float csm_normal_bias = ve::CSM_NORMAL_BIAS;
	float depth_bias_constant = ve::SHADOW_DEPTH_BIAS_CONSTANT;
	float depth_bias_slope = ve::SHADOW_DEPTH_BIAS_SLOPE;
	float depth_bias_clamp = ve::SHADOW_DEPTH_BIAS_CLAMP;
	ShadowCullMode shadow_cull_mode = ShadowCullMode::Front;
	Topology topology = Topology::TRIANGLE_LIST;
	RenderMode render_mode = RenderMode::BRDF_MICROFACET;
	bool hdr_enabled = false;
	bool msaa = false;
	bool vsync = false;

	// lighting
	glm::vec3 ambient_light_color = glm::vec3(1.0f);
	float ambient_light_intensity = 0.006f;
	bool ibl_enabled = true;
	float ibl_diffuse_intensity = 0.2f;
	float ibl_specular_intensity = 0.2f;
	float ibl_min_ambient = 0.005f;
	bool ibl_auto_exposure = true;

	// passes
	bool geometry_prepass_enabled = true;
	bool shadow_mask_enabled = true;
	bool shadow_mask_half_res = false;
	bool gtao_enabled = true;
	bool gtao_half_res = true;
	float gtao_radius = 0.25f;
	float gtao_intensity = 0.5f;
	bool ssr_enabled = true;
	bool ssr_half_res = true;
	int ssr_max_steps = 48;
	float ssr_thickness = 0.3f;
	float ssr_max_roughness = 0.85f;
	float ssr_max_distance = 25.0f;

	// culling
	bool enable_frustum_culling = true;
	CullingBackendMode culling_backend = CullingBackendMode::CPU;
	bool hiz_occlusion_enabled = false;
	// fallback: enable the GpuCullingSystem to use for shadows
	bool meshlet_object_culled_shadows = false;
	int min_parallel_cull_entities = static_cast<int>(ve::MIN_PARALLEL_CULL_ENTITIES);

	// lod
	int lod_force_level = -1;
	float lod_screen_thresholds[3] = {0.3f, 0.15f, 0.05f};
	float lod_hysteresis = 0.2f;

	// post process
	int blur_radius = 0;
	float blur_strength = 1.0f;
	float exposure = 1.0f;
	int tone_map_mode = TONEMAP_GT;
	float hdr_peak_white = 4.0f;
	bool bloom_enabled = true;
	float bloom_strength = 0.01f;

	// debug overlays (renderer-respected toggles)
	bool show_axes = false;
	bool show_aabb_debug = false;
	bool show_skinned_points = false;
	bool show_area_lights = false;

	// profiling
	bool gpu_profiling = false;
};

} // namespace ve