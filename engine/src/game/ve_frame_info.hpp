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
#include <map>

namespace ve {

struct VENGINE_API PointLight {
	glm::vec4 position;
	glm::vec4 color; // w indicates light intensity
};

struct VENGINE_API UniformBufferObject {
	glm::mat4 view;
	glm::mat4 proj;
	glm::vec4 ambient_light_color = DEFAULT_AMBIENT_LIGHT_COLOR;
	PointLight point_lights[ve::MAX_LIGHTS];
	alignas(16) uint32_t num_lights = 0;
	// reminder: alignment
};

struct VENGINE_API VeFrameInfo {
	vk::raii::DescriptorSet& global_descriptor_set;
	vk::raii::DescriptorSet& material_descriptor_set;
	vk::raii::DescriptorSet& cubemap_descriptor_set; // For skybox rendering
	vk::raii::CommandBuffer& command_buffer;
	vk::raii::CommandBuffer& compute_command_buffer; // For compute shaders
	std::unordered_map<uint32_t, VeGameObject>& game_objects;
	float frame_time;
	float total_time;
	uint32_t current_frame;
};
}