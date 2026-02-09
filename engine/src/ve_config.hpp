#pragma once
#include <vector>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_beta.h> // for VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#include <glm/glm.hpp>

namespace ve {

constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
constexpr glm::vec4 DEFAULT_AMBIENT_LIGHT_COLOR = glm::vec4(1.0f, 1.0f, 1.0f, 0.016f); // w indicates light intensity
constexpr uint32_t MAX_LIGHTS = 20; // requirded for UBO alignment
constexpr uint32_t MAX_SHADOW_LIGHTS = 3; // Maximum number of shadow-casting lights (independent of MAX_LIGHTS)

constexpr bool MSAA_ENABLED = true;
#ifdef __APPLE__
	constexpr bool ENABLE_RAY_TRACING = false; // not supported on moltenVK
#else
	constexpr bool ENABLE_RAY_TRACING = false;
#endif

// Shadow mapping configuration
constexpr uint32_t SHADOW_MAP_RESOLUTION = 2048*2;
constexpr float SHADOW_BIAS = 0.0001f;

// Central list of required Vulkan device extensions
inline const std::vector<const char*> REQUIRED_DEVICE_EXTENSIONS = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	//VK_KHR_SPIRV_1_4_EXTENSION_NAME, promoted to core in Vulkan 1.2
	//VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, promoted to core in Vulkan 1.3
	//VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, promoted to core in Vulkan 1.3
#if defined(__APPLE__)
	VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, // required for portability on macOS
#endif
#if ENABLE_RAY_TRACING
	VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
	VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
	VK_KHR_RAY_QUERY_EXTENSION_NAME,
#endif
	// Ray tracing extensions (not on moltenvk yet)
	//VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	//VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
	//VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
	//VK_KHR_RAY_QUERY_EXTENSION_NAME
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

}// namespace ve

// TODO: Windows: test separate transfer queue on Windows with a discrete GPU
// TODO: Consdider consolidating index and vertex buffer into single buffer and use offsets
// TODO: consider moving the timeline semaphore from VeSwapChain somewhere else
// TODO: Centralise frame time measurement code (e.g. in Sandbox/imgui_layer)
// TODO: Fix flashbang upon startup of veapp