/* App-side fireworks effect.
 *
 * Registers 6 sub-emitters on the shared ParticleBackend (rocket, streamer,
 * trail, smoke, spark, flash) and tracks per-rocket CPU state
 */
#pragma once
#include "events/event_bus.hpp"
#include "rendering/particle_backend.hpp"
#include "rendering/particle_emitter_params.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"
#include "scene/ve_entity.hpp"

#include <glm/glm.hpp>
#include <filesystem>
#include <memory>
#include <vector>

namespace ve {
class SceneManager;
struct SceneUnloadedEvent;
}

namespace ve::effects {

struct FireworksConfig {
	glm::vec3 launch_pos{0.0f};
	float launch_vel_min = 60.0f;
	float launch_vel_max = 110.0f;
	float launch_spread_xy = 15.0f;
	int launch_count = 1;

	glm::vec4 particle_color{1.0f};
	bool use_random_color = true;

	int explosion_particle_count = 60;
	float explosion_size = 0.8f;

	glm::vec3 wind_direction{1.0f, 1.0f, 0.0f};
	float wind_strength = 3.0f;
	float gravity = 15.0f;

	float trail_interval = 0.012f;
	glm::vec4 smoke_color = glm::vec4(0.4f, 0.4f, 0.4f, 0.5f);

	float streamer_drag = 0.9f;
	float streamer_life_min = 2.7f;
	float streamer_life_max = 3.1f;
	float streamer_burst_speed = 45.0f;
	float streamer_burst_spread = 5.0f;
	float streamer_brightness = 10.0f;

	// Trail (particles dropped behind each streamer)
	float trail_drag = 0.9f;
	bool  trail_life_proportional = true;
	float trail_life_fraction     = 0.6f;
	float trail_life_min          = 4.0f;
	float trail_life_max          = 5.0f;

	// Apex flash (giant transient particle spawned at rocket explosion)
	float flash_peak_scale = 170.0f;

	// Transient point light spawned at each explosion
	float flash_peak_intensity = 7000.0f;
	float flash_life = 0.5f;
};

class Fireworks {
public:
	Fireworks(ParticleBackend& backend,
	          VeResourceManager& resources,
	          SceneManager& scene_manager,
	          EventBus& event_bus,
	          std::filesystem::path fire_atlas_path = {},
	          std::filesystem::path smoke_atlas_path = {},
	          FireworksConfig config = {});
	~Fireworks();

	Fireworks(const Fireworks&) = delete;
	Fireworks& operator=(const Fireworks&) = delete;

	// Per-frame: updates active rockets (CPU physics + spiral wobble), emits
	// trail/smoke at rocket position, fires streamer burst on rocket death.
	void update(float dt, float total_time);

	void launchRocket();
	void launchRocket(glm::vec3 pos, glm::vec3 vel, glm::vec4 color);

	void drawConfigUI();

	FireworksConfig& config() { return m_config; }
	const FireworksConfig& config() const { return m_config; }

private:
	struct Rocket {
		glm::vec3 pos;
		glm::vec3 vel;
		glm::vec4 color;
		float timer;
		float trail_timer;
	};

	struct TransientLight {
		Entity entity;
		float life;
		float max_life;
		float peak_intensity;
	};

	void onSceneUnloaded(const SceneUnloadedEvent& e);

	ParticleBackend& m_backend;
	SceneManager& m_scene_manager;
	EventBus& m_event_bus;
	EventSubscriptionId m_scene_unloaded_sub = 0;
	FireworksConfig m_config;

	ResourceHandle<VeTexture> m_fire_atlas;
	ResourceHandle<VeTexture> m_smoke_atlas;
	uint32_t m_fire_atlas_slot  = ROUND_MASK_SENTINEL;
	uint32_t m_smoke_atlas_slot = ROUND_MASK_SENTINEL;

	// Sub-emitter ids on the partic backend
	EmitterId m_trail_emitter    = INVALID_EMITTER;
	EmitterId m_smoke_emitter    = INVALID_EMITTER;
	EmitterId m_spark_emitter    = INVALID_EMITTER;
	EmitterId m_streamer_emitter = INVALID_EMITTER;
	EmitterId m_rocket_emitter   = INVALID_EMITTER;
	EmitterId m_flash_emitter    = INVALID_EMITTER;

	std::vector<Rocket> m_rockets;
	std::vector<TransientLight> m_lights;
};

} // namespace ve::effects
