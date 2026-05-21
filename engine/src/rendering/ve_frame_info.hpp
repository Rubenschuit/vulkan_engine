#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "scene/ve_entity.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_scene.hpp"
#include "scene/camera_view.hpp"
#include "rendering/culling/culling_system.hpp"

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>
#include <glm/glm.hpp>
#include <vector>

namespace ve {

// --- Rendering mode enums ---

enum class RenderMode : uint32_t {
	BRDF = 0,
	NORMAL_VECTOR = 1,
	TANGENT_VECTOR = 2,
	BITANGENT_VECTOR = 3,
	NORMAL_MAP = 4,
	BRDF_MICROFACET = 5, // Standard
	CSM_CASCADE = 6,
	CLUSTER_HEATMAP = 7,
	LOD_LEVEL = 8,
	MESHLET_ID = 9,
};

enum class ShadowMode : uint32_t {
	DISABLED = 0,
	REGULAR = 1,
	PCF = 2,
	PCSS = 3,
};

enum ToneMapMode : int {
	TONEMAP_NONE         = 0,
	TONEMAP_REINHARD     = 1,
	TONEMAP_ACES         = 2,
	TONEMAP_PBR_NEUTRAL  = 3,
	TONEMAP_GT           = 4,
};

// --- GPU-side light structs (layouts must match shader bindings) ---

struct PointLight {
	alignas(16) glm::vec4 position; // xyz = world position, w = range (0 = infinite)
	alignas(16) glm::vec4 color;    // xyz = color * intensity, w = intensity
};

struct DirectionalLight {
	alignas(16) glm::vec4 direction;  // xyz = direction toward surface, w = unused
	alignas(16) glm::vec4 color;      // xyz = color * intensity, w = intensity
};

struct SpotLight {
	alignas(16) glm::vec4 position;   // xyz = world pos, w = range
	alignas(16) glm::vec4 direction;  // xyz = normalized dir, w = cos(outerConeAngle)
	alignas(16) glm::vec4 color;      // xyz = color * intensity, w = cos(innerConeAngle)
};

struct ShadowLight {
	alignas(16) glm::mat4 light_view;
	alignas(16) glm::mat4 light_proj;
	alignas(16) glm::mat4 shadow_matrix;        // bias * light_proj * light_view
	alignas(16) glm::vec4 light_index_padding;  // x = light_index, y = type (0=point, 1=directional, 2=spot)
	alignas(16) glm::vec4 atlas_bounds;         // xy = min UV, zw = max UV
};

// --- Per-instance SSBO data uploaded each frame, indexed by gl_InstanceIndex ---

struct InstanceData {
	alignas(16) glm::mat4 transform;
	alignas(16) glm::mat3x4 normal_transform;
	alignas(4)  uint32_t material_index;
	alignas(4)  uint32_t lod_level;
	alignas(4)  float    depth_offset;     // clip-space Z offset for MASK
	alignas(4)  uint32_t material_flags;   // see MaterialFlag in ve_config.hpp
};
static_assert(sizeof(InstanceData) == 128, "InstanceData must be 128 bytes for SSBO alignment");

// --- Main UBO bound to every render pass at set 0 ---

struct UniformBufferObject {
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
	alignas(16) glm::mat4 projection_view;
	alignas(16) glm::vec4 camera_position;
	alignas(16) glm::vec4 ambient_light_color = DEFAULT_AMBIENT_LIGHT_COLOR;
	alignas(16) PointLight point_lights[ve::MAX_LIGHTS];
	alignas(16) ShadowLight shadow_lights[ve::MAX_SHADOW_LIGHTS];
	alignas(4)  uint32_t num_lights = 0;
	alignas(4)  uint32_t num_shadow_lights = 0;
	alignas(4)  RenderMode render_mode = RenderMode::BRDF;
	alignas(4)  ShadowMode shadow_mode = ShadowMode::REGULAR;
	alignas(4)  float shadow_bias = ve::SHADOW_BIAS;
	alignas(4)  uint32_t num_dir_lights = 0;
	alignas(16) DirectionalLight dir_lights[ve::MAX_DIR_LIGHTS];

	// Cascaded shadow maps
	alignas(16) glm::mat4 csm_shadow_matrices[ve::NUM_CSM_CASCADES];
	alignas(16) glm::vec4 csm_split_distances{};   // view-space far-Z per cascade
	alignas(4)  uint32_t csm_cascade_count = 0;
	alignas(4)  uint32_t csm_base_layer = 0;
	alignas(4)  float    csm_shadow_map_size = 4096.0f; // atlas width (set by LightSystem)
	alignas(4)  uint32_t csm_dir_light_index = 0xFFFFFFFF; // dir_lights[] index that owns CSM (0xFFFFFFFF = none)
	alignas(4)  float    pcss_light_size = 0.04f;  // world-space light radius for PCSS penumbra
	alignas(4)  uint32_t csm_blend_dithered = 0;   // 0 = off, 1 = linear, 2 = dithered
	alignas(4)  float    csm_normal_bias = ve::CSM_NORMAL_BIAS;

	// Screen-space shadow mask reprojection
	alignas(16) glm::mat4 inverse_projection_view{1.0f};
	alignas(16) glm::mat4 prev_projection_view{1.0f};
	alignas(8)  glm::vec2 screen_size{};

	// Spot lights
	alignas(16) uint32_t num_spot_lights = 0;
	alignas(16) SpotLight spot_lights[ve::MAX_SPOT_LIGHTS];

	// IBL
	alignas(4) float    ibl_diffuse_intensity = 0.0f;
	alignas(4) uint32_t prefiltered_mip_levels = 1;
	alignas(4) float    ibl_specular_intensity = 0.0f;
	alignas(4) float    ibl_min_ambient = 0.0f;
	alignas(16) glm::vec4 sh_coefficients[9]{};
};
static_assert(offsetof(UniformBufferObject, dir_lights) % 16 == 0,
	"dir_lights must be 16-byte aligned for GPU UBO layout");
static_assert(offsetof(UniformBufferObject, sh_coefficients) % 16 == 0,
	"sh_coefficients must be 16-byte aligned for GPU UBO layout");

// --- Push constants ---

struct PostProcessPushConstant {
	int   blur_radius = 0;       // 0 means no blur
	float blur_strength = 1.0f;
	float exposure = 1.0f;
	int   color_space = 0;       // 0: SRGB, 1: Extended Linear, 2: HDR10 ST2084
	float bloom_strength = 0.01f;
	int   tone_map_mode = TONEMAP_GT;
	float hdr_peak_white = 4.0f; // GT tonemap peak brightness in scene-linear units (HDR only)
	float padding;
	glm::vec2 texel_size;
};
static_assert(sizeof(PostProcessPushConstant) == 40,
	"PostProcessPushConstant size must match shader push constant layout");

// --- CPU-side per-frame structs ---

// Sub-region of the shadow atlas occupied by one CSM cascade or point/spot light.
struct FrameAtlasRegion {
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t resolution = 0;
};

// Per-cascade view/proj plus bounding sphere, filled by LightSystem and read by ShadowRenderSystem.
struct CsmCascadeData {
	glm::mat4 light_view[ve::NUM_CSM_CASCADES];
	glm::mat4 light_proj[ve::NUM_CSM_CASCADES];
	uint32_t  active_cascade_count = 0;
	glm::vec3 center[ve::NUM_CSM_CASCADES]{};
	float     radius[ve::NUM_CSM_CASCADES]{};
};

// --- Per-frame rendering context passed to every render system ---

struct VeFrameInfo {
	// Command buffers
	vk::raii::CommandBuffer* command_buffer;
	vk::raii::CommandBuffer& compute_command_buffer;
	vk::raii::CommandBuffer* compute2_command_buffer = nullptr;

	// Descriptor sets bound during scene rendering
	vk::raii::DescriptorSet& global_descriptor_set;                  // set 0 (UBO + per-instance SSBO + material SSBO)
	vk::raii::DescriptorSet* shadow_mask_descriptor_set = nullptr;   // set 3 (null when mask unavailable)
	vk::raii::DescriptorSet* cluster_descriptor_set = nullptr;       // set 4 (null when clustering disabled)
	vk::raii::DescriptorSet* ao_descriptor_set = nullptr;            // set 5 (dummy white when AO disabled)
	vk::raii::DescriptorSet* ibl_descriptor_set = nullptr;           // set 6 (dummy black when IBL unavailable)
	vk::raii::DescriptorSet& cubemap_descriptor_set;
	vk::raii::DescriptorSet& shadow_descriptor_set;
	vk::raii::DescriptorSet* cpu_global_descriptor_set = nullptr;

	// Scene
	VeScene*   active_scene = nullptr;
	Registry*  registry = nullptr;
	CameraView camera_view;
	Entity     selected_entity = Entity::null();

	// Per-frame state
	uint32_t current_frame;
	float    frame_time;
	float    total_time;

	// CPU culling output
	std::vector<VisibleObject>& visible_objects;

	// Instance SSBO (persistently mapped, render systems append into it)
	InstanceData* instance_data = nullptr;
	uint32_t      instance_count = 0;
	uint32_t      instance_capacity = 0;

	// Shadow rendering
	ShadowMode shadow_mode = ShadowMode::REGULAR;
	float      depth_bias_constant = ve::SHADOW_DEPTH_BIAS_CONSTANT;
	float      depth_bias_slope = ve::SHADOW_DEPTH_BIAS_SLOPE;
	float      depth_bias_clamp = 0.0f;
	CsmCascadeData csm_data;
	const FrameAtlasRegion* shadow_atlas_regions = nullptr;  // MAX_SHADOW_LAYERS entries
	uint32_t shadow_atlas_width = 0;
	uint32_t shadow_atlas_height = 0;
	const uint32_t* csm_cascade_resolutions = nullptr; // NUM_CSM_CASCADES entries
	bool     shadow_mask_active = false;  // true when mask pipeline variant should be used

	// Post-processing
	PostProcessPushConstant post_process_push;

	// GPU-driven culling toggles
	bool gpu_culling_active = false;
	bool meshlet_culling_active = false;

	// Pre-skinning output (vertices written to a scratch VBO before draws)
	const class SkinningPrePass* skinning_pre_pass = nullptr;

	vk::raii::CommandBuffer& cmd() const {
		assert(command_buffer && "command_buffer is null");
		return *command_buffer;
	}
};

}
