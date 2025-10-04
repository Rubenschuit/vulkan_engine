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

	struct SimplePushConstantData {
		glm::mat4 transform;
		alignas(16) glm::vec3 color{1.0f};
	};

	struct UniformBufferObject {
		glm::mat4 view;
		glm::mat4 proj;
		alignas(16) glm::vec3 offset;
	};

	struct VeFrameInfo {
		vk::raii::DescriptorSet& global_descriptor_set;
		vk::raii::CommandBuffer& command_buffer;
		std::unordered_map<uint32_t, VeGameObject>& game_objects;
		float frame_time;
	};
}