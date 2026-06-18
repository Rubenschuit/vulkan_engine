/* Compute pre-pass that deforms per-entity vertices (morph targets + skinning)
 *
 * For each entity that needs deform (MeshComponent + SkinComponent and/or
 * MorphComponent), a slot is allocated in the dynamic
 * region of PbrMegaBuffer's mega VBO + mega shadow VBO. The compute shader
 * reads each entity's bind-pose vertices from the static
 * region, applies morph (weighted deltas) then skinning (joint palette), and
 * writes into that slot.
 */
#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"
#include "scene/ve_entity.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {
	class VeDevice;
	class VeBuffer;
	class VeDescriptorPool;
	class VeDescriptorSetLayout;
	class VeComputePipeline;
	class Registry;
	class EventBus;
	class PbrMegaBuffer;
	class MeshComponent;
	class VeMesh;
	class GpuSceneManager;
	class VeThreadPool;
}

namespace ve {

class VENGINE_API DeformPrePass {
public:
	DeformPrePass(VeDevice& device, VeDescriptorPool& descriptor_pool,
	                std::filesystem::path shader_path, EventBus& event_bus);
	~DeformPrePass();

	DeformPrePass(const DeformPrePass&) = delete;
	DeformPrePass& operator=(const DeformPrePass&) = delete;

	void updatePalette(Registry& registry, uint32_t frame_index, VeThreadPool* thread_pool);

	// Writes m_deformed_offset_ssbos[frame_index][gpu_id] = absolute vertex offset
	// in the mega VBO dynamic region for every entity that has a slot AND is
	// registered with GpuSceneManager.
	void updateDeformedOffsets(const GpuSceneManager& gpu_scene,
	                           const PbrMegaBuffer& mega_buffer,
	                           uint32_t frame_index);

	void dispatch(VeFrameInfo& fi, PbrMegaBuffer& mega_buffer);

	// Returns the absolute offset (in vertices) into the mega VBO + mega shadow VBO
	// where this entity's post-skin output sits for the given frame, or UINT32_MAX
	// if the entity has no active slot
	static constexpr uint32_t INVALID_OFFSET = UINT32_MAX;
	uint32_t getDeformedVertexOffset(Entity entity, uint32_t frame_index,
	                                 const PbrMegaBuffer& mega_buffer) const;

	VeBuffer& getDeformedOffsetBuffer(uint32_t frame_index) const {
		return *m_deformed_offset_ssbos[frame_index];
	}

private:
	struct SubspaceSlot {
		uint32_t offset;        // in [0, MAX_DEFORMED_VERTICES_PER_FRAME)
		uint32_t vertex_count;
	};

	struct DeformDispatch {
		Entity entity;
		uint32_t vertex_count;
		uint32_t palette_offset; // in 4x4 matrices
		uint32_t deform_flags;   // DEFORM_HAS_SKIN | DEFORM_HAS_MORPH
		uint32_t target_count;
		uint32_t weight_offset;
	};

	void createSetLayout();
	void createPipelineLayout();
	void createPipeline();
	void subscribeToRegistry(Registry& registry);
	void seedDeformableSlots(Registry& registry);
	void refreshDescriptors(uint32_t frame_index, PbrMegaBuffer& mega_buffer);

	bool allocateSlotForEntity(Entity entity, VeMesh* mesh);
	void freeSlotForEntity(Entity entity);
	void resetAllocator();

	VeDevice& m_ve_device;
	VeDescriptorPool& m_descriptor_pool;
	std::filesystem::path m_shader_path;
	Registry* m_registry = nullptr;

	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_palette_ssbos;

	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_morph_weight_ssbos;

	// One entry per dispatched workgroup
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_wg_info_ssbos;

	// gpu_id -> absolute vertex offset in the mega VBO dynamic region; 0 means "no
	// live slot, cull shader falls back to the object's bind-pose vertex_offset".
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_deformed_offset_ssbos;
	std::array<std::vector<uint32_t>, MAX_FRAMES_IN_FLIGHT> m_last_written_gpu_ids;

	std::unique_ptr<VeDescriptorSetLayout> m_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;

	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptor_sets{
		vk::raii::DescriptorSet{nullptr}, vk::raii::DescriptorSet{nullptr}};
	// Mega-buffer generation each frame's descriptor set was last written against
	std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> m_cached_mega_generation{};

	std::unordered_map<uint32_t, SubspaceSlot> m_slots; // keyed by entity.index()
	std::vector<SubspaceSlot> m_free_ranges;            // sorted by offset, disjoint

	// Per-frame dispatch list, populated by updatePalette and consumed by dispatch.
	std::array<std::vector<DeformDispatch>, MAX_FRAMES_IN_FLIGHT> m_pending_dispatches;

	// Deformable (skin/morph) entities gathered each frame by updatePalette.
	std::vector<Entity> m_deform_scratch;
};

} // namespace ve