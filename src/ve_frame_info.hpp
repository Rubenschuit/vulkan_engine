/* This file contains definitions of data structures needed
   for each frame in the rendering process. */
#pragma once
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>
#include <glm/glm.hpp>
#include "ve_model.hpp"

namespace ve {

	struct UniformBufferObject {
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;
	};

	struct VeFrameInfo {
		vk::raii::DescriptorSet& global_descriptor_set;
		vk::raii::CommandBuffer& command_buffer;
		VeModel& ve_model;
	};
}