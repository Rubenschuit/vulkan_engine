/* This file contains definitions of data structures needed
for each frame in the rendering process. */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_model.hpp"
#include "scene/ve_game_object.hpp"
#include "scene/ve_scene.hpp"
#include "ve_config.hpp"
#include "scene/ve_camera.hpp"

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>
#include <glm/glm.hpp>
#include <unordered_map>

namespace ve {

struct PointLight {
	glm::vec4 position;
	glm::vec4 color; // w indicates light intensity
};

struct ShadowLight {
	glm::mat4 light_view;
	glm::mat4 light_proj;
	glm::mat4 shadow_matrix;        // pre-computed bias * light_proj * light_view
	glm::vec4 light_index_padding;  // x = light_index, yzw = padding (ensures 16-byte alignment)
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

struct PostProcessPushConstant {
	int blur_radius = 0; // 0 means no blur
	float blur_strength = 1.0f;
	float exposure = 1.0f;
	int color_space = 0; // 0: SRGB, 1: Extended Linear, 2: HDR10 ST2084
	float bloom_strength = 0.01f;
	float padding[3];
	glm::vec2 texel_size;
};

struct BloomDownsamplePushConstant {
	int is_first_pass;
};

struct BloomUpsamplePushConstant {
	float filter_radius;
};

struct UniformBufferObject {
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 projection_view;
	glm::vec4 camera_position;
	glm::vec4 ambient_light_color = DEFAULT_AMBIENT_LIGHT_COLOR;
	PointLight point_lights[ve::MAX_LIGHTS]; // reserved for MAX_LIGHTS point lights
	ShadowLight shadow_lights[ve::MAX_SHADOW_LIGHTS]; // reserved for MAX_SHADOW_LIGHTS shadow-casting lights
	uint32_t num_lights = 0; // actual number of point lights ( <= MAX_LIGHTS)
	uint32_t num_shadow_lights = 0; // actual number of shadow-casting lights ( <= MAX_SHADOW_LIGHTS)
	RenderMode render_mode = RenderMode::BRDF;
	ShadowMode shadow_mode = ShadowMode::REGULAR;
	float shadow_bias = ve::SHADOW_BIAS;
};

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
	std::unordered_map<uint32_t, VeGameObject>& game_objects;
	float frame_time;
	float total_time;
	uint32_t current_frame;
	PostProcessPushConstant post_process_push;
};

}