#pragma once

#include <vector>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_beta.h> // for VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#include <glm/glm.hpp>

namespace ve {
	constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

	constexpr glm::vec4 DEFAULT_AMBIENT_LIGHT_COLOR = glm::vec4(1.0f, 1.0f, 1.0f, 0.02f); // w indicates light intensity
	constexpr uint32_t MAX_LIGHTS = 100; // requirded for UBO alignment

	//constexpr vk::PresentModeKHR PRESENT_MODE = vk::PresentModeKHR::eMailbox;
	constexpr vk::PresentModeKHR PRESENT_MODE = vk::PresentModeKHR::eImmediate;

	// Central list of required Vulkan device extensions
	inline const std::vector<const char*> REQUIRED_DEVICE_EXTENSIONS = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_SPIRV_1_4_EXTENSION_NAME,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
		VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME // required for portability on macOS
	};

	// Central list of required Vulkan instance extensions (GLFW-required are added at runtime)
	inline const std::vector<const char*> REQUIRED_INSTANCE_EXTENSIONS = {
		VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME // required for portability subset
	};

	// Central list of validation layers
	inline const std::vector<const char*> VALIDATION_LAYERS = {
		"VK_LAYER_KHRONOS_validation"
	};
	enum class QueueKind { Graphics, Transfer };
}

// TODO: Windows: test separate transfer queue on Windows with a discrete GPU
// TODO: Consdider consolidating index and vertex buffer into single buffer and use offsets
// TODO: Make sure the directories are setup correctly
// TODO: consider moving the timeline semaphore from VeSwapChain somewhere else