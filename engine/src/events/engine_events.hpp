/* Engine event types for the EventBus.
 *
 * These are distinct from ECS events (ecs_event_dispatcher.hpp) which are
 * scoped to a Registry lifetime. Engine events span scenes and cover
 * cross-system communication.
 */
#pragma once
#include "events/event_bus.hpp"
#include "scene/ve_entity.hpp"
#include "rendering/ve_frame_info.hpp"

#include <cstdint>
#include <filesystem>
#include <glm/vec3.hpp>
#include <string>

namespace ve {

class Registry;

// ── Scene lifecycle ─────────────────────────────────────────────────────────

struct SceneLoadedEvent {
	Registry* registry;
};

struct SceneUnloadedEvent {};

// ── Asset loading ───────────────────────────────────────────────────────────

struct AssetLoadCompleteEvent {
	std::string model_name;
	std::filesystem::path source_path;
};

// ── Physics collisions ──────────────────────────────────────────────────────

struct CollisionEvent {
	Entity entity_a;
	Entity entity_b;
	glm::vec3 contact_point;
	glm::vec3 contact_normal;
	float penetration_depth;
};

struct CollisionEndEvent {
	Entity entity_a;
	Entity entity_b;
};

// ── Input actions ───────────────────────────────────────────────────────────

struct InputActionEvent {
	enum class Action : uint8_t {
		ResetParticles,
		LaunchFirework,
		SetMode,
		ResetDisc,
		ToggleUI,
		TogglePerformanceUI
	};
	Action action;
	uint32_t value = 0;
};

// ── Settings changes ────────────────────────────────────────────────────────

class VeDescriptorPool;

struct DepthBiasChangedEvent {};

struct TopologyChangedEvent {
	Topology topology;
};

struct ShadowSamplesChangedEvent {
	uint32_t pcf_samples;
	uint32_t pcss_filter_samples;
};

struct ShadowMaskResolutionChangedEvent : ImmediateOnly {
	VeDescriptorPool& pool;
	vk::Extent2D mask_extent;
	vk::Extent2D depth_extent;
	const vk::raii::ImageView& depth_image_view;
	vk::Image depth_image;
};

struct GtaoResolutionChangedEvent : ImmediateOnly {
	VeDescriptorPool& pool;
	vk::Extent2D ao_extent;
	vk::Extent2D depth_extent;
	const vk::raii::ImageView& depth_image_view;
	vk::Image depth_image;
};

struct GtaoParametersChangedEvent {
	float radius;
	float intensity;
};

struct DepthPrePassChangedEvent {
	bool enabled;
};

struct GpuShadowFallbackChangedEvent {
	bool enabled;
};

struct ClusterEnabledChangedEvent {
	bool enabled;
};

// ── Rendering state changes ────────────────────────────────────────────────

struct BackendChangedEvent {};

struct SkyboxChangedEvent {
	std::filesystem::path skybox_path;
};

struct PipelineRecreateEvent {
	vk::Format offscreen_format;
	vk::SampleCountFlagBits sample_count;
};

struct ResolutionChangedEvent : ImmediateOnly {
	VeDescriptorPool& pool;
	vk::Extent2D extent;
	vk::Format swap_chain_format;
	vk::Format offscreen_format;
	const vk::raii::ImageView& resolve_target_view;
	const vk::raii::ImageView& depth_image_view;
	vk::Image depth_image;
	const vk::raii::ImageView& wboit_accum_view;
	const vk::raii::ImageView& wboit_revealage_view;
	bool shadow_mask_half_res;
	bool gtao_half_res;
};

} // namespace ve