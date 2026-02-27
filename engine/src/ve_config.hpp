#pragma once
#include <vector>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_beta.h> // for VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#include <glm/glm.hpp>

namespace ve {

constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

// lights
constexpr glm::vec4 DEFAULT_AMBIENT_LIGHT_COLOR = glm::vec4(1.0f, 1.0f, 1.0f, 0.04f); // w indicates light intensity
constexpr uint32_t MAX_LIGHTS = 160; // requirded for UBO alignment
constexpr uint32_t MAX_POINT_SHADOW_LIGHTS = 2; // Max point lights that can cast shadows
constexpr uint32_t MAX_SPOT_LIGHTS = 32;
constexpr uint32_t MAX_SPOT_SHADOW_LIGHTS = 2;
constexpr uint32_t MAX_SHADOW_LIGHTS = MAX_POINT_SHADOW_LIGHTS + MAX_SPOT_SHADOW_LIGHTS;
constexpr uint32_t MAX_DIR_LIGHTS = 4; // Maximum number of directional lights

// Celestial billboard (sun/moon) configuration
constexpr float CELESTIAL_DISTANCE = 200.0f;
constexpr float CELESTIAL_SCALE = 22.0f;
constexpr float CELESTIAL_INTENSITY_BOOST = 100.0f; // Intensity multiplier so bloom creates halo/corona

// Shadow mapping configuration
constexpr uint32_t SHADOW_MAP_RESOLUTION = 2048; // unified resolution for all shadow layers
constexpr float SHADOW_BIAS = 0.00042f;
constexpr float CSM_NORMAL_BIAS = 0.08f; // world-space normal offset for CSM, scaled per cascade
constexpr float DIR_SHADOW_MAX_DISTANCE = 300.0f; // Max distance from camera for directional light shadows

// Cascaded Shadow Maps (CSM) configuration
constexpr uint32_t NUM_CSM_CASCADES = 3; // keep in sync with shader
constexpr uint32_t CSM_SHADOW_MAP_RESOLUTION = SHADOW_MAP_RESOLUTION; // per-cascade resolution
constexpr float CSM_SPLIT_LAMBDA = 0.80f; // practical split blend (0=linear, 1=logarithmic)
constexpr uint32_t MAX_SHADOW_LAYERS = NUM_CSM_CASCADES + MAX_POINT_SHADOW_LIGHTS + MAX_SPOT_SHADOW_LIGHTS; // total shadow map array layers
static_assert(SHADOW_MAP_RESOLUTION == CSM_SHADOW_MAP_RESOLUTION,
	"Shadow map resolution and CSM resolution must match (unified shadow array)");

// Clustered forward shading
constexpr uint32_t CLUSTER_TILE_SIZE = 64;           // screen-space tile size in pixels
constexpr uint32_t CLUSTER_Z_SLICES = 24;            // depth slices (logarithmic distribution)
constexpr uint32_t MAX_LIGHTS_PER_CLUSTER = 64;      // max lights assignable per cluster
constexpr uint32_t MAX_CLUSTER_LIGHTS = 1024;        // max total point lights for cluster path
constexpr float CLUSTER_LIGHT_CUTOFF = 0.005f;       // intensity fraction for effective range (range=0 lights)
constexpr float CLUSTER_MAX_EFFECTIVE_RANGE = 500.0f; // cap for derived effective range

// LOD (Level of Detail) configuration
constexpr uint32_t MAX_LOD_LEVELS = 4;           // LOD 0 = full, LOD 1..3 = simplified
constexpr float LOD_RATIOS[] = {1.0f, 0.5f, 0.25f, 0.125f};  // target triangle ratio per LOD
constexpr float LOD_ERROR_THRESHOLD = 0.01f;     // meshoptimizer simplification error threshold
constexpr float LOD_SCREEN_THRESHOLDS[] = {0.3f, 0.15f, 0.05f}; // screen fraction: LOD 0->1, 1->2, 2->3
constexpr float LOD_HYSTERESIS = 0.2f;             // 20% band to prevent LOD oscillation
constexpr uint32_t LOD_MIN_TRIANGLES = 64;        // never simplify below this triangle count
static_assert(std::size(LOD_RATIOS) == MAX_LOD_LEVELS,
              "LOD_RATIOS must have MAX_LOD_LEVELS entries");
static_assert(std::size(LOD_SCREEN_THRESHOLDS) == MAX_LOD_LEVELS - 1,
              "LOD_SCREEN_THRESHOLDS must have MAX_LOD_LEVELS-1 entries");

// Multi-threaded command recording
constexpr uint32_t MAX_RENDER_WORKERS = 8;
constexpr uint32_t MIN_PARALLEL_GROUPS = 128; // below this, prefer single-threaded
constexpr uint32_t MIN_PARALLEL_CULL_ENTITIES = 64; // below this, single-threaded culling

// Bindless textures + Multi-Draw Indirect
constexpr uint32_t MAX_BINDLESS_TEXTURES = 16384;
constexpr uint32_t MAX_GPU_MATERIALS = 8192;

// GPU-driven culling
constexpr uint32_t MAX_GPU_OBJECTS = 16384;
constexpr uint32_t GPU_CULL_WORKGROUP_SIZE = 256;
constexpr uint32_t GPU_CULL_BUCKET_COUNT = 4; // non-MASK back, non-MASK double, MASK back, MASK double

constexpr bool MSAA_ENABLED = true;
#ifdef __APPLE__
	constexpr bool ENABLE_RAY_TRACING = false; // not supported on moltenVK
#else
	constexpr bool ENABLE_RAY_TRACING = false;
#endif

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
};

// Central list of validation layers
inline const std::vector<const char*> VALIDATION_LAYERS = {
	"VK_LAYER_KHRONOS_validation"
};

}// namespace ve
