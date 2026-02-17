/* This file contains definitions of data structures needed
for each frame in the rendering process. */
#pragma once
#include "ve_export.hpp"
#include "scene/ve_entity.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_scene.hpp"
#include "ve_config.hpp"
#include "scene/ve_camera.hpp"

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>
#include <glm/glm.hpp>
#include <vector>

namespace ve {

class MeshComponent;

// Per-instance transform data uploaded to the instance SSBO each frame.
// Indexed by gl_InstanceIndex (SV_InstanceID in Slang) in vertex shaders.
struct InstanceData {
	alignas(16) glm::mat4 transform;            // 64 bytes — world transform
	alignas(16) glm::mat3x4 normal_transform;   // 48 bytes — normal matrix (3 columns packed as vec4)
};
static_assert(sizeof(InstanceData) == 112, "InstanceData must be 112 bytes for SSBO alignment");

// Cached visible object built once per frame by the culling system.
// Stores Entity + direct MeshComponent* for zero-lookup rendering.
struct VisibleObject {
	Entity entity;
	MeshComponent* mesh = nullptr;
};

struct PointLight {
	alignas(16) glm::vec4 position; // xyz = world position, w = range (0 = infinite)
	alignas(16) glm::vec4 color;    // xyz = color * intensity, w = intensity
};

struct DirectionalLight {
	alignas(16) glm::vec4 direction;  // xyz = direction toward surface, w = unused
	alignas(16) glm::vec4 color;      // xyz = color * intensity, w = intensity
};

struct ShadowLight {
	alignas(16) glm::mat4 light_view;
	alignas(16) glm::mat4 light_proj;
	alignas(16) glm::mat4 shadow_matrix;        // pre-computed bias * light_proj * light_view
	alignas(16) glm::vec4 light_index_padding;  // x = light_index, y = type (0=point, 1=directional), zw = padding
};

enum class RenderMode : uint32_t {
	BRDF = 0,
	NORMAL_VECTOR = 1,
	TANGENT_VECTOR = 2,
	BITANGENT_VECTOR = 3,
	NORMAL_MAP = 4,
	BRDF_MICROFACET = 5,
};

enum class ShadowMode : uint32_t {
	DISABLED = 0,
	REGULAR = 1,
	PCF = 2,
};

enum class Topology : uint32_t {
	TRIANGLE_LIST = 0,
	LINE_LIST = 1,
};

enum ToneMapMode : int {
	TONEMAP_NONE         = 0,
	TONEMAP_REINHARD     = 1,
	TONEMAP_ACES         = 2,
	TONEMAP_PBR_NEUTRAL  = 3,
	TONEMAP_GT           = 4,
};

struct PostProcessPushConstant {
	int blur_radius = 0; // 0 means no blur
	float blur_strength = 1.0f;
	float exposure = 1.0f;
	int color_space = 0; // 0: SRGB, 1: Extended Linear, 2: HDR10 ST2084
	float bloom_strength = 0.01f;
	int tone_map_mode = TONEMAP_NONE;
	float hdr_peak_white = 4.0f; // GT tonemap peak brightness in scene-linear units (HDR only)
	float padding;
	glm::vec2 texel_size;
};

struct BloomDownsamplePushConstant {
	int is_first_pass;
};

struct BloomUpsamplePushConstant {
	float filter_radius;
};

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
};
static_assert(offsetof(UniformBufferObject, dir_lights) % 16 == 0,
	"dir_lights must be 16-byte aligned for GPU UBO layout");
static_assert(sizeof(PostProcessPushConstant) == 40,
	"PostProcessPushConstant size must match shader push constant layout");

struct VeFrameInfo {
	vk::raii::DescriptorSet& global_descriptor_set;
	vk::raii::DescriptorSet& texture_descriptor_set;
	vk::raii::DescriptorSet& material_descriptor_set;
	VeScene* active_scene = nullptr;  // For per-object descriptor set lookup
	vk::raii::DescriptorSet& cubemap_descriptor_set;
	vk::raii::DescriptorSet& shadow_descriptor_set;
	vk::raii::CommandBuffer& command_buffer;
	vk::raii::CommandBuffer& compute_command_buffer;
	ve::VeCamera& camera;
	Registry* registry = nullptr;
	std::vector<VisibleObject>& visible_objects;
	float frame_time;
	float total_time;
	uint32_t current_frame;
	PostProcessPushConstant post_process_push;
	// Instance data SSBO: persistently mapped, render systems append transforms here.
	InstanceData* instance_data = nullptr;  // mapped pointer to instance buffer
	uint32_t instance_count = 0;            // current number of instances written this frame
	uint32_t instance_capacity = 0;         // max instances the buffer can hold

	// GPU timing: compute systems write the start timestamp after their barriers resolve.
	vk::QueryPool compute_query_pool = VK_NULL_HANDLE;
	uint32_t compute_start_query = 0;       // query index for compute start timestamp
};

}