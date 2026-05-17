/*
* Shared GPU particle pool: particle SSBO ping-pong, GPU free list
* with deferred recycle queue, and indirect-draw compaction. One backend
* instance is shared across the whole app; emitters write into it by tagging
* SpawnEvent.emitter_id.
*
* Only emitParticles() is thread-safe.
*/
#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "rendering/ve_frame_info.hpp"
#include "rendering/particle_emitter_params.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_compute_pipeline.hpp"

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace ve {

class EventBus;

struct SpawnEvent {
	glm::vec4 position_scale;    // xyz = position, w = scale (<=0 to use emitter default)
	glm::vec4 velocity_life;     // xyz = base velocity, w = life (<=0 to randomize from emitter)
	glm::vec4 color;             // rgba
	uint32_t count;              // number of particles to spawn
	uint32_t emitter_id;
	uint32_t _pad0 = 0u;
	uint32_t _pad1 = 0u;
};
static_assert(sizeof(SpawnEvent) == 64, "SpawnEvent must be 64 bytes; layout is shared with particle_common.slangh");

// Global per-frame parameters. Per-emitter state lives in the EmitterParams[]
// SSBO; this UBO carries only fields that apply to every emitter.
struct ParticleParams {
	glm::vec4 frustum_planes[6]; // xyz = normal, w = distance
	float delta_time;
	float total_time = 0.0f;
	uint32_t capacity;
	uint32_t reset; // 1 = reset particles this dispatch
	uint32_t seed;  // rng seed
	uint32_t spawn_event_count;
	uint32_t frame_id; // monotonic counter
	uint32_t _pad0 = 0u;
};

struct Particle {
	glm::vec4 position; // w is scale
	glm::vec4 velocity = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // w is life
	glm::vec4 color;
	glm::vec4 tex_coords;

	uint32_t atlas_index;
	float max_life;
	float blend;
	uint32_t emitter_id;

	float trail_accumulator;
	float spawn_scale;
	float _pad0 = 0.0f;
	float _pad1 = 0.0f;
};
static_assert(sizeof(Particle) % 16 == 0, "Particle must be 16-byte aligned");

struct RenderParticle {
	glm::vec4 position;
	glm::vec4 color;
	glm::vec4 tex_coords;
	uint32_t atlas_index;
	float    blend;
	uint32_t _pad0 = 0u;
	uint32_t _pad1 = 0u;

	static std::vector<vk::VertexInputBindingDescription> getBindingDescription() {
		return { { 0, sizeof(RenderParticle), vk::VertexInputRate::eInstance } };
	}

	static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptions() {
		return {
			{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(RenderParticle, position)},
			{.location = 1, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(RenderParticle, color)},
			{.location = 2, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(RenderParticle, tex_coords)},
			{.location = 3, .binding = 0, .format = vk::Format::eR32Uint,            .offset = offsetof(RenderParticle, atlas_index)},
			{.location = 4, .binding = 0, .format = vk::Format::eR32Sfloat,          .offset = offsetof(RenderParticle, blend)}
		};
	}
};
static_assert(sizeof(RenderParticle) % 16 == 0, "RenderParticle must be 16-byte aligned");

struct ParticleBackendCreateInfo {
	VeDevice& device;
	std::shared_ptr<VeDescriptorPool> descriptor_pool;
	const vk::raii::DescriptorSetLayout& global_set_layout;

	// Default texture used to populate every slot in the bindless atlas array
	ResourceHandle<VeTexture> default_atlas;

	vk::Format color_format;
	vk::SampleCountFlagBits sample_count;

	uint32_t capacity;
	std::filesystem::path shader_path;
	EventBus* event_bus = nullptr;
};

class VENGINE_API ParticleBackend {
public:
	explicit ParticleBackend(const ParticleBackendCreateInfo& info);
	~ParticleBackend();

	ParticleBackend(const ParticleBackend&) = delete;
	ParticleBackend& operator=(const ParticleBackend&) = delete;

	void recordComputeCommands(VeFrameInfo& frame_info);
	void render(VeFrameInfo& frame_info) const;

	void setEnabled(bool enabled) { m_enabled = enabled; }
	bool isEnabled() const { return m_enabled; }
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
		m_pipeline.reset();
		createPipeline(color_format, sample_count);
	}
	// Schedule a GPU reset on the next dispatch: wipes all slots and reseeds the free list.
	void scheduleRestart();

	// Register a texture into a free slot of the particle bindless array.
	// Returns the slot index.
	uint32_t registerAtlas(ResourceHandle<VeTexture> atlas);
	void releaseAtlas(uint32_t slot);
	void setDefaultAtlas(ResourceHandle<VeTexture> atlas);

	EmitterId registerEmitter(const EmitterParams& params);
	void updateEmitter(EmitterId id, const EmitterParams& params);
	void releaseEmitter(EmitterId id);
	const EmitterParams& getEmitterParams(EmitterId id) const;
	EmitterParams& getEmitterParams(EmitterId id);

	void setSpeed(float speed) { m_speed = speed; }
	float getSpeed() const { return m_speed; }

	void setCapacity(uint32_t count);
	uint32_t getCapacity() const { return m_capacity; }

	// Queue a SpawnEvent. Slot allocation happens GPU-side in the spawn pre-pass.
	void emitParticles(SpawnEvent event);

	void stageCapacity(uint32_t count);
	uint32_t getPendingCapacity() const { return m_pending_capacity; }
	bool hasPendingCapacity() const { return m_pending_capacity != m_capacity; }
	void applyStagedCapacity();
	void applyPendingResize();

private:
	static constexpr uint32_t MAX_SPAWN_EVENTS = 4096;
	static constexpr size_t MAX_PENDING_SPAWNS = MAX_SPAWN_EVENTS * 4;

	void createDescriptorSetLayouts();
	void createShaderStorageBuffers();
	void createSpawnEventBuffers();
	void createFreeListBuffers();
	void createUniformBuffers();
	void createEmitterParamsBuffers();
	void createDescriptorSets();
	void createComputePipelineLayout();
	void createComputePipeline();
	void createSpawnComputePipeline();
	void createRecycleComputePipeline();
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& texture_set_layout);
	void createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);
	void createRenderDescriptorSet();
	void writeAtlasSlot(uint32_t slot); // re-issue descriptor write for one slot

	VeDevice& m_ve_device;

	uint32_t m_capacity = 0;   // active count = allocated buffer capacity
	uint32_t m_pending_capacity = 0;
	bool m_apply_pending = false;
	float m_total_time = 0.0f;
	bool m_pending_reset = false;
	uint32_t m_reset_seed{0};
	float m_speed{1.0f};
	bool m_enabled{true};
	uint32_t m_frame_id{0};

	std::vector<EmitterParams> m_emitter_storage;
	std::vector<uint8_t> m_emitter_used;
	std::vector<EmitterId> m_emitter_free_list;

	std::mutex m_pending_spawns_mutex;
	std::deque<SpawnEvent> m_pending_spawns;

	std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;
	std::unique_ptr<VeDescriptorSetLayout> m_render_set_layout;

	// Per-frame resources
	std::vector<std::unique_ptr<VeBuffer>> m_compute_uniform_buffers;  // small UBO per frame
	std::vector<std::unique_ptr<VeBuffer>> m_shader_storage_buffers; // large SSBO per frame
	std::vector<std::unique_ptr<VeBuffer>> m_render_buffers; // packed SSBO for rendering
	std::vector<std::unique_ptr<VeBuffer>> m_indirect_buffers; // Indirect draw commands
	std::vector<std::unique_ptr<VeBuffer>> m_spawn_storage_buffers; // spawn event SSBO per frame
	std::vector<std::unique_ptr<VeBuffer>> m_emitter_params_buffers; // emitter params SSBO per frame
	std::vector<vk::raii::DescriptorSet> m_compute_descriptor_sets;

	// slot_pool_counters: [FREE_LIST_TOP_INDEX]=free-list top, [RECYCLE_QUEUE_COUNT_INDEX]=recycle-queue fill.
	std::unique_ptr<VeBuffer> m_slot_pool_counters_buffer;
	std::unique_ptr<VeBuffer> m_free_slot_stack_buffer;  // LIFO of free slot ids
	std::unique_ptr<VeBuffer> m_recycle_queue_buffer;    // per-frame queue of slots that died this update
	std::unique_ptr<VeBuffer> m_free_slot_stack_staging_buffer;
	std::shared_ptr<VeDescriptorPool> m_descriptor_pool;

	// Bindless atlas array
	ResourceHandle<VeTexture> m_default_atlas_handle;
	std::array<ResourceHandle<VeTexture>, MAX_PARTICLE_ATLASES> m_atlas_slots;
	std::vector<uint32_t> m_free_atlas_slots; // populated in reverse so registerAtlas hands out low indices first
	vk::raii::DescriptorSet m_render_descriptor_set{nullptr};

	std::filesystem::path  m_shader_path;
	vk::raii::PipelineLayout m_compute_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;
	std::unique_ptr<VeComputePipeline> m_spawn_compute_pipeline;
	std::unique_ptr<VeComputePipeline> m_recycle_compute_pipeline;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_pipeline;
};

} // namespace ve
