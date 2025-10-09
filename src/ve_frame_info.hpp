/* This file contains definitions of data structures needed
   for each frame in the rendering process. */
#pragma once
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>
#include <glm/glm.hpp>
#include <map>

#include "ve_model.hpp"
#include "ve_game_object.hpp"

namespace ve {

	const glm::vec3 DEFAULT_LIGHT_POSITION = glm::vec3(-10.0f, 10.0f, 10.0f);
	const glm::vec4 DEFAULT_LIGHT_COLOR = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
	const glm::vec4 DEFAULT_AMBIENT_LIGHT_COLOR = glm::vec4(1.0f, 1.0f, 1.0f, 0.02f); // w indicates light intensity

	struct UniformBufferObject {
		glm::mat4 view;
		glm::mat4 proj;
		glm::vec4 ambient_light_color = DEFAULT_AMBIENT_LIGHT_COLOR;
		glm::vec4 light_color = DEFAULT_LIGHT_COLOR;
		alignas(16) glm::vec3 light_position = DEFAULT_LIGHT_POSITION;
	};

	struct VeFrameInfo {
		vk::raii::DescriptorSet& global_descriptor_set;
		vk::raii::DescriptorSet& material_descriptor_set;
		vk::raii::CommandBuffer& command_buffer;
		std::unordered_map<uint32_t, VeGameObject>& game_objects;
		float frame_time;
	};
}