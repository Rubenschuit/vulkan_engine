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
#include <string_view>

namespace ve {

class Registry;
class VeScene;

// ── Scene lifecycle ─────────────────────────────────────────────────────────

struct SceneLoadedEvent {
	Registry* registry;
	VeScene* scene;
};

struct SceneUnloadedEvent {};

// ── Asset loading ───────────────────────────────────────────────────────────

struct AssetLoadCompleteEvent {
	std::string model_name;
	std::filesystem::path source_path;
	Entity wrapper_entity;
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
	std::string_view name;
	uint32_t value = 0;
};

// ── Material changes ────────────────────────────────────────────────────────

struct MaterialDataChangedEvent {
	uint32_t gpu_index;
};

// ── Resource lifecycle ──────────────────────────────────────────────────────

// Emitted by VeResourceManager just before doUnload runs.
template <typename T>
struct ResourceUnloadingEvent : ImmediateOnly {
	T* resource;
};

// ── Settings changes ────────────────────────────────────────────────────────

struct DepthBiasChangedEvent {};

struct TopologyChangedEvent {
	Topology topology;
};

struct ShadowSamplesChangedEvent {
	uint32_t pcf_samples;
	uint32_t pcss_filter_samples;
};

struct GtaoParametersChangedEvent {
	float radius;
	float intensity;
};

struct DepthPrePassChangedEvent {
	bool enabled;
};

struct ClusterEnabledChangedEvent {
	bool enabled;
};

// ── Rendering state changes ────────────────────────────────────────────────

// Emitted by VeRenderer after its swap chain has been recreated
struct SwapChainRecreatedEvent {};

// Emitted by VeRenderer when the scene-render extent changes. RenderPipeline
// reacts and emits ResolutionChangedEvent (engine-internal, render_events.hpp)
// including all the relevant data for subscribers.
struct ViewportResizedEvent {};

struct SkyboxChangedEvent {
	std::filesystem::path skybox_path;
};

} // namespace ve