#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "core/ve_buffer.hpp"
#include "core/ve_descriptors.hpp"
#include "game/ve_frame_info.hpp"
#include "core/ve_pipeline.hpp"
#include "core/ve_compute_pipeline.hpp"

#include <memory>
#include <vector>
#include <atomic>
#include <atomic>
#include <deque>

namespace ve {

// not enum class because we pass to shader
enum ParticleResetKind : uint32_t {
	POINT = 1,
	DISC = 2
};

enum ParticleMode : int32_t {
	GRAVITY_EARTH = 1,
	COOL = 2,
	SUCC = 3,
	STASIS = 4,
	GALAXY_MASSIVE = 5,
};

enum ParticleType : uint32_t {
	TYPE_DEFAULT = 0,
	TYPE_SMOKE = 1,
	TYPE_SPARK = 2,
	TYPE_ROCKET = 3,
	TYPE_EXPLOSION = 4,
	TYPE_EXPLOSION_SMOKE = 5
};

struct SpawnEvent {
	glm::vec4 position_scale;    // xyz = position, w = scale
	glm::vec4 velocity_life;     // xyz = velocity, w = life
	glm::vec4 color;             // rgba
	glm::uvec4 info;             // x = start_index, y = count, z = type, w = variance (as float bits)
};

// Uniform buffer with parameters for compute shader
struct ParticleParams {
	float delta_time;
	float total_time = 0.0f;
	uint32_t particle_count;
	uint32_t reset; // 1 = reset particles this dispatch
	uint32_t seed;  // rng seed for reset
	float mean;
	float stddev;
	uint32_t reset_kind; // see ParticleResetKind enum
	uint32_t mode; // see ParticleMode enum
	alignas(16) glm::vec4 origin; // w unused
	uint32_t row_count;
	float min_life;
	float max_life;
	uint32_t should_respawn;
	// ring buffer emission
	uint32_t spawn_event_count;
	float gravity;
	uint32_t padding1;
	uint32_t padding2;
	alignas(16) glm::vec4 wind_direction; // xyz = direction, w = intensity
};

// Data structure for vertex shader input
struct Particle {
	glm::vec4 position; // w is scale
	glm::vec4 velocity; // w is life
	glm::vec4 color;
	glm::vec4 tex_coords;
	glm::vec4 extra_data; // x = type, yzw = unused/padding

	static std::vector<vk::VertexInputBindingDescription> getBindingDescription() {
		// Per-instance particle attributes (position, color)
		return { { 0, sizeof(Particle), vk::VertexInputRate::eInstance } };
	}

	// we dont need velocity for rendering
	static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptions() {
		return {
			vk::VertexInputAttributeDescription( 0, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Particle, position) ),
			vk::VertexInputAttributeDescription( 1, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Particle, color) ),
			vk::VertexInputAttributeDescription( 2, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Particle, tex_coords) ),
			vk::VertexInputAttributeDescription( 3, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Particle, extra_data) )
		};
	}
};

class VENGINE_API ParticleSystem {
public:
	ParticleSystem(
		VeDevice& device,
		std::shared_ptr<VeDescriptorPool> descriptor_pool,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& texture_set_layout,
		vk::Format color_format,
		uint32_t particle_count,
		glm::vec3 origin,
		std::filesystem::path shader_path,
		bool start_active = true);
	~ParticleSystem();

	ParticleSystem(const ParticleSystem&) = delete;
	ParticleSystem& operator=(const ParticleSystem&) = delete;

	void update(VeFrameInfo& frame_info);
	void render(VeFrameInfo& frame_info) const;
	void scheduleRestart(); // schedule GPU reset of particle positions
	void setMode(uint32_t mode) { m_mode = mode; }
	void setSpeed(float speed) { m_speed = speed; }
	void setOrigin(const glm::vec3& origin) { m_origin = origin; }
	void resetPoint() { m_reset_kind = 1u; scheduleRestart(); }
	void resetDisc() { m_reset_kind = 2u; scheduleRestart(); }

	// Change particle count; recreates storage buffers and descriptor sets
	void setParticleCount(uint32_t count, bool reset = true);
	void setMean(float mean) { m_mean = mean;}
	void setStddev(float stddev) { m_stddev = stddev;}
	uint32_t getParticleCount() const { return m_particle_count; }
	uint32_t getCapacity() const { return m_capacity; }
	float getMean() const { return m_mean; }
	float getStddev() const { return m_stddev; }
	float getSpeed() const { return m_speed; }
	void setWind(const glm::vec4& wind) { m_wind_direction = wind; }
	glm::vec4 getWind() const { return m_wind_direction; }
	void setLifeRange(float min, float max) { m_min_life = min; m_max_life = max; }
	float getMinLife() const { return m_min_life; }
	float getMaxLife() const { return m_max_life; }
	void setShouldRespawn(bool respawn) { m_should_respawn = respawn; }
	bool getShouldRespawn() const { return m_should_respawn; }
	void setGravity(float gravity) { m_gravity = gravity; }
	float getGravity() const { return m_gravity; }

	// Legacy emit (single batch), might want to remove
	void emitParticles(uint32_t count);
	// ring-buffer emit
	void emitParticles(SpawnEvent info);

	// Pending/staged particle count UI helpers
	void stageParticleCount(uint32_t count);
	uint32_t getPendingParticleCount() const { return m_pending_particle_count; }
	bool hasPendingParticleCount() const { return m_pending_particle_count != m_particle_count; }
	void applyStagedParticleCount();


private:
	static constexpr uint32_t MAX_SPAWN_EVENTS = 4096;

	void createDescriptorSetLayouts();
	void createShaderStorageBuffers();
	void createSpawnBuffers();
	void createUniformBuffers();
	void createDescriptorSets();
	void createComputePipelineLayout();
	void createComputePipeline();
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& texture_set_layout);
	void createPipeline(vk::Format color_format);

	void ensureCapacity(uint32_t needed);

	VeDevice& m_ve_device;

	float m_mean = 0.0f;
	float m_stddev = 5.0f;
	uint32_t m_particle_count = 0;   // active count
	uint32_t m_capacity = 0;         // allocated particle capacity for buffers
	uint32_t m_pending_particle_count = 0; // UI-staged value
	float m_total_time = 0.0f;
	glm::vec3 m_origin{0.0f, 0.0f, 10.0f};
	std::atomic<bool> m_pending_reset{false}; // atomic not necessary (no multi-threading yet)
	uint32_t m_reset_seed{0};
	uint32_t m_reset_kind{ParticleResetKind::POINT}; // see ParticleResetKind enum
	uint32_t m_mode{ParticleMode::COOL}; // see ParticleMode enum
	float m_speed{1.0f};
	float m_gravity{9.81f};
	float m_min_life{40.0f};
	float m_max_life{80.0f};
	glm::vec4 m_wind_direction{0.0f}; // default no wind
	bool m_should_respawn{true};

	// Emission state
	uint32_t m_emit_head{0}; // Current head of the ring buffer (in particle index)
	std::deque<SpawnEvent> m_pending_spawns;

	// Descriptor layouts for this system
	std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;

	// Per-frame resources
	std::vector<std::unique_ptr<VeBuffer>> m_compute_uniform_buffers;  // small UBO per frame
	std::vector<std::unique_ptr<VeBuffer>> m_shader_storage_buffers; // large SSBO per frame
	std::vector<std::unique_ptr<VeBuffer>> m_spawn_storage_buffers; // spawn event SSBO per frame
	std::vector<vk::raii::DescriptorSet> m_compute_descriptor_sets;


	// Shared pool for descriptor allocations
	std::shared_ptr<VeDescriptorPool> m_descriptor_pool;

	std::filesystem::path  m_shader_path;
	vk::raii::PipelineLayout m_compute_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_pipeline;
};

} // namespace ve
