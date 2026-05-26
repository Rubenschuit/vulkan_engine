#pragma once
#include <array>
#include <vector>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_beta.h> // for VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#include <glm/glm.hpp>

namespace ve {

constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

// Helper to create a std::array<T, N> where every element is T{nullptr}.
// Avoids hardcoding the element count when MAX_FRAMES_IN_FLIGHT changes.
template <typename T, size_t N = MAX_FRAMES_IN_FLIGHT>
auto makeNullArray() {
	return []<size_t... Is>(std::index_sequence<Is...>) {
		return std::array<T, N>{ (static_cast<void>(Is), T{nullptr})... };
	}(std::make_index_sequence<N>{});
}

// lights
constexpr glm::vec4 DEFAULT_AMBIENT_LIGHT_COLOR = glm::vec4(1.0f, 1.0f, 1.0f, 0.04f); // w indicates light intensity
constexpr uint32_t MAX_LIGHTS = 320; // requirded for UBO alignment
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
constexpr float SHADOW_BIAS = 0.00042f;
constexpr float CSM_NORMAL_BIAS = 0.08f; // world-space normal offset for CSM, scaled per cascade
constexpr float SHADOW_DEPTH_BIAS_CONSTANT = 1.25f;
constexpr float SHADOW_DEPTH_BIAS_SLOPE = 1.75f;
constexpr float DIR_SHADOW_MAX_DISTANCE = 300.0f; // Max distance from camera for directional light shadows

// Cascaded Shadow Maps (CSM) configuration
constexpr uint32_t NUM_CSM_CASCADES = 3; // keep in sync with shader
constexpr uint32_t CSM_CASCADE_RESOLUTIONS[NUM_CSM_CASCADES] = {2048, 1024, 1024}; // per-cascade atlas resolution (Medium preset defaults)
constexpr float CSM_SPLIT_LAMBDA = 0.80f; // practical split blend (0=linear, 1=logarithmic)
constexpr int32_t CSM_SCROLL_THRESHOLD = 256; // max texels per frame before fallback to full re-render
constexpr float CSM_Z_MARGIN = 150.0f; // depth margin behind cascade sphere for shadow casters
constexpr uint32_t POINT_SHADOW_RESOLUTION = 512; // per-light atlas resolution (Medium preset default)
constexpr uint32_t SPOT_SHADOW_RESOLUTION = 512;
constexpr uint32_t MAX_SHADOW_LAYERS = NUM_CSM_CASCADES + MAX_POINT_SHADOW_LIGHTS + MAX_SPOT_SHADOW_LIGHTS;

enum class ShadowResolutionPreset : uint32_t {
	LOW    = 0,
	MEDIUM = 1,
	HIGH   = 2,
	ULTRA  = 3,
};
constexpr uint32_t SHADOW_RESOLUTION_PRESET_COUNT = 4;

struct ShadowResolutionPresetValues {
	uint32_t csm[NUM_CSM_CASCADES];
	uint32_t point;
	uint32_t spot;
};

constexpr ShadowResolutionPresetValues SHADOW_RESOLUTION_PRESETS[SHADOW_RESOLUTION_PRESET_COUNT] = {
	{{1024,  512,  512},  256,  256},
	{{2048, 1024, 1024},  512,  512},
	{{4096, 2048, 2048}, 1024, 1024},
	{{8192, 4096, 4096}, 2048, 2048},
};

inline const ShadowResolutionPresetValues& getShadowResolutionPreset(ShadowResolutionPreset p) {
	uint32_t idx = static_cast<uint32_t>(p);
	if (idx >= SHADOW_RESOLUTION_PRESET_COUNT)
		idx = static_cast<uint32_t>(ShadowResolutionPreset::MEDIUM);
	return SHADOW_RESOLUTION_PRESETS[idx];
}

// Shadow cull pass mode (matches CullParams::is_shadow_pass in shaders)
enum class ShadowPassMode : uint32_t {
	ALL_OBJECTS  = 1,
	STATIC_ONLY  = 2,
	DYNAMIC_ONLY = 3,
};

// Primitive topology selection for the main PBR pass (wireframe debug).
enum class Topology : uint32_t {
	TRIANGLE_LIST = 0,
	LINE_LIST = 1,
};

// Clustered forward lighting
constexpr uint32_t CLUSTER_TILE_SIZE = 64;            // screen-space tile size in pixels
constexpr uint32_t CLUSTER_Z_SLICES = 24;             // depth slices (logarithmic distribution)
constexpr uint32_t MAX_LIGHTS_PER_CLUSTER = 64;       // max lights assignable per cluster
constexpr uint32_t MAX_CLUSTER_LIGHTS = 1024;         // max total point lights for cluster path
constexpr float CLUSTER_LIGHT_CUTOFF = 0.005f;        // intensity fraction for effective range (range=0 lights)
constexpr float CLUSTER_MAX_EFFECTIVE_RANGE = 500.0f; // cap for derived effective range

// LOD (Level of Detail) configuration
constexpr uint32_t MAX_LOD_LEVELS = 4;           // LOD 0 = full, LOD 1..3 = simplified
constexpr float LOD_RATIOS[] = {1.0f, 0.5f, 0.25f, 0.125f};  // target triangle ratio per LOD
constexpr float LOD_ERROR_THRESHOLD = 0.01f;     // meshoptimizer simplification error threshold
constexpr float LOD_SCREEN_THRESHOLDS[] = {0.70f, 0.35f, 0.15f}; // screen fraction: LOD 0->1, 1->2, 2->3
constexpr float LOD_HYSTERESIS = 0.2f;             // 20% band to prevent LOD oscillation
constexpr uint32_t LOD_MIN_TRIANGLES = 64;        // never simplify below this triangle count
static_assert(std::size(LOD_RATIOS) == MAX_LOD_LEVELS,
              "LOD_RATIOS must have MAX_LOD_LEVELS entries");
static_assert(std::size(LOD_SCREEN_THRESHOLDS) == MAX_LOD_LEVELS - 1,
              "LOD_SCREEN_THRESHOLDS must have MAX_LOD_LEVELS-1 entries");

// Multi-threaded command recording
constexpr uint32_t MAX_RENDER_WORKERS = 8;
constexpr uint32_t MIN_PARALLEL_CULL_ENTITIES = 64; // below this, single-threaded culling

// Bindless textures + Multi-Draw Indirect
constexpr uint32_t MAX_BINDLESS_TEXTURES = 16384;
constexpr uint32_t MAX_GPU_MATERIALS = 8192;

// Material flags (bits 0-5, stored in MaterialGPU::material_flags and ObjectDataGPU::material_flags)
namespace MaterialFlag {
	constexpr uint32_t ALPHA_MODE_MASK = 0x3;  // bits 0-1: 0=OPAQUE, 1=MASK, 2=BLEND
	constexpr uint32_t DOUBLE_SIDED    = 0x4;  // bit 2
	constexpr uint32_t FLIP_TEX_V      = 0x8;  // bit 3
	constexpr uint32_t SPEC_GLOSS      = 0x10; // bit 4
	constexpr uint32_t HAS_TEXTURE     = 0x20; // bit 5 (MaterialGPU only: has albedo texture)
}

// Object flags (per-object rendering behavior, stored in ObjectDataGPU::object_flags)
namespace ObjectFlag {
	constexpr uint32_t IS_TRANSPARENT = 0x1; // bit 0: alpha_mode == BLEND
	constexpr uint32_t NO_SHADOW      = 0x2; // bit 1
	constexpr uint32_t DYNAMIC        = 0x4; // bit 2
}

// GPU-driven culling
constexpr uint32_t MAX_GPU_OBJECTS = 16384*4; // sync with shader
constexpr uint32_t GPU_CULL_WORKGROUP_SIZE = 256;
constexpr uint32_t GPU_CULL_BUCKET_COUNT = 6; // 0=opaque back, 1=opaque double, 2=mask back, 3=mask double, 4=blend back, 5=blend double
constexpr uint32_t MAX_DRAW_GROUPS = 8192;         // up to MAX_LOD_LEVELS draw groups per unique mesh+material combo
constexpr uint32_t MAX_LOD_INSTANCE_SLOTS = MAX_GPU_OBJECTS * MAX_LOD_LEVELS; // worst-case instance buffer size

// Hi-Z occlusion culling
constexpr uint32_t MAX_HIZ_MIPS = 13;

// Dynamic region of the mega VBO that holds skinning compute outputs.
// Reserves 2 (ping-pong) × MAX_SKINNED_VERTICES_PER_FRAME × (48 + 12) bytes in
// the mega VBO + shadow VBO.
constexpr uint32_t MAX_SKINNED_VERTICES_PER_FRAME = 256 * 1024;

// Meshlet culling (keep in sync with ve_constants.slangh)
constexpr uint32_t MESHLET_MAX_VERTICES        = 64;
constexpr uint32_t MESHLET_MAX_TRIANGLES       = 124;
constexpr uint32_t MAX_MESHLET_DRAWS           = 262144*4;   // total indirect commands across all buckets
constexpr uint32_t MESHLET_CULL_WORKGROUP_SIZE = 256;
constexpr uint32_t MESHLET_BUCKET_COUNT        = 6;        // 0=opaque-back, 1=opaque-double, 2=mask-back, 3=mask-double, 4=blend-back, 5=blend-double
constexpr uint32_t MESHLET_SHADOW_BUCKET_COUNT = 4;        // shadows skip transparent buckets 4-5
constexpr uint32_t MAX_MESHLET_SHADOW_DRAWS    = 262144*4;   // total indirect commands across all shadow buckets per layer
// gpu_id and local meshlet index are packed into VkDrawIndexedIndirectCommand::firstInstance as two 16-bit fields.
static_assert(MAX_GPU_OBJECTS <= 65536,
	"MAX_GPU_OBJECTS exceeds 16-bit range; meshlet firstInstance packing would corrupt gpu_id");

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
	// VK_KHR_portability_subset is added at runtime if required (MoltenVK)
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
};

// Central list of validation layers
inline const std::vector<const char*> VALIDATION_LAYERS = {
	"VK_LAYER_KHRONOS_validation"
};

}// namespace ve
