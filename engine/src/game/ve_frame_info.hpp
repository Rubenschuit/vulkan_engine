/* This file contains definitions of data structures needed
for each frame in the rendering process. */
#pragma once
#include "ve_export.hpp"
#include "ve_model.hpp"
#include "ve_game_object.hpp"
#include "ve_config.hpp"

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>
#include <glm/glm.hpp>
#include <unordered_map>

namespace ve {

struct PointLight {
	glm::vec4 position;
	glm::vec4 color; // w indicates light intensity
	glm::mat4 light_view;          // light's view matrix
	glm::mat4 light_proj;          // light's projection matrix
};

enum RenderMode {
	BRDF = 0,
	NORMAL_VECTOR = 1,
	TANGENT_VECTOR = 2,
	BITANGENT_VECTOR = 3,
	NORMAL_MAP = 4,
	BRDF_MICROFACET = 5,
};

enum ShadowMode {
	DISABLED = 0,
	REGULAR = 1,
	PCF = 2,
};

struct UniformBufferObject {
	glm::mat4 view;
	glm::mat4 proj;
	glm::vec4 camera_position;
	glm::vec4 ambient_light_color = DEFAULT_AMBIENT_LIGHT_COLOR;
	PointLight point_lights[ve::MAX_LIGHTS]; // reserved for MAX_LIGHTS point lights
	uint32_t num_lights = 0; // actual number of point lights ( <= MAX_LIGHTS)
	uint32_t render_mode = RenderMode::BRDF;
	uint32_t shadow_mode = ShadowMode::REGULAR;
	float shadow_bias = ve::SHADOW_BIAS;
	// reminder: alignment
};

struct VeFrameInfo {
	vk::raii::DescriptorSet& global_descriptor_set;
	vk::raii::DescriptorSet& material_descriptor_set;
	vk::raii::DescriptorSet& cubemap_descriptor_set;
	vk::raii::DescriptorSet& shadow_descriptor_set;
	vk::raii::CommandBuffer& command_buffer;
	vk::raii::CommandBuffer& compute_command_buffer;
	std::unordered_map<uint32_t, VeGameObject>& game_objects;
	float frame_time;
	float total_time;
	uint32_t current_frame;
};

}