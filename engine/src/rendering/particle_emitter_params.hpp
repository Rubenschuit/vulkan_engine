#pragma once
#include "ve_export.hpp"
#include <glm/glm.hpp>
#include <cstdint>

// Per-emitter parameters indexed by emitter_id (stored in Particle.emitter_id
// and SpawnEvent.emitter_id). The ParticleBackend uploads EmitterParams[] each
// frame; the compute shader reads them per particle.
namespace ve {

using EmitterId = uint32_t;

// Initial-velocity distribution applied by the spawn pre-pass.
enum class SpawnDistribution : uint32_t {
	Gaussian = 0u,       // vel += N(mean, stddev^2) per axis
	UniformSphere = 1u,  // vel += uniform-direction * (mean + stddev*rand01)
};

// Sentinel atlas_index: skip the texture sample and render the procedural
// round mask (soft bloom + radial fade).
constexpr uint32_t ROUND_MASK_SENTINEL = 0xFFFFFFFEu;

constexpr float FLOOR_DISABLED           = -1.0e30f;
constexpr float FLOOR_DISABLED_THRESHOLD = -1.0e29f;

struct VENGINE_API EmitterParams {
	glm::vec4 origin{0.0f, 0.0f, 0.0f, 0.0f};         // w = unused
	glm::vec4 wind{0.0f, 0.0f, 0.0f, 0.0f};           // xyz = direction (normalized), w = strength
	glm::vec4 color_start{1.0f, 1.0f, 1.0f, 1.0f};    // rgba at life=max (life_frac=1)
	glm::vec4 color_end{1.0f, 1.0f, 1.0f, 1.0f};      // rgba at life=0

	// Physics
	float gravity = 9.81f;
	float drag = 0.0f;                                 // velocity damping per second
	float central_attractor = 0.0f;                    // accel toward origin
	float tangential_strength = 0.0f;                  // tangential accel around y axis
	float floor_z = FLOOR_DISABLED;

	// Lifetime range
	float min_life = 1.0f;
	float max_life = 4.0f;

	// Spawn velocity distribution
	float mean = 0.0f;
	float stddev = 5.0f;

	// Sub-emitter chaining (-1 = unused). Backend ids; set programmatically.
	int32_t on_tick_emitter_id = -1;
	int32_t on_tick2_emitter_id = -1;
	float tick_probability = 1.0f;
	float tick2_probability = 0.0f;
	float trail_interval = 0.004f;

	// Sub-emitter spawn settings
	float spawn_scale = 0.0f;            // 0 = inherit parent.position.w; >0 = absolute
	float spawn_velocity_scale = 0.1f;   // child.velocity = parent.velocity * this
	float spawn_life_fraction = 0.5f;    // child.life = parent.life * this; 0 = use this emitter's min_life/max_life

	// Atlas texture
	uint32_t row_count = 8;
	uint32_t atlas_one_shot = 0; // 0 = loop, 1 = one-shot clamped at last frame
	uint32_t atlas_index = ROUND_MASK_SENTINEL; // index into texture array; ROUND_MASK_SENTINEL = procedural round mask

	SpawnDistribution spawn_distribution = SpawnDistribution::Gaussian;

	float brightness = 1.0f;

	// Scale-over-life. disabled when <= 0
	float scale_end = -1.0f;

	uint32_t _pad0 = 0u;
};

static_assert(sizeof(EmitterParams) == 160, "EmitterParams must be 160 bytes (16-aligned)");

constexpr uint32_t MAX_EMITTERS = 256;
constexpr EmitterId INVALID_EMITTER = 0xFFFFFFFFu;

// Sync with fragment shader
// Bounded by VkPhysicalDeviceLimits::maxPerStageDescriptorSamplers
// (= 16 on Apple M1).
constexpr uint32_t MAX_PARTICLE_ATLASES = 16;



} // namespace ve