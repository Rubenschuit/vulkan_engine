/* Compute pre-pass that skins per-entity vertex streams.
 *
 * For each entity with MeshComponent + SkinComponent, a slot is allocated in the
 * dynamic region of PbrMegaBuffer's mega VBO + mega shadow VBO (one ping-pong
 * per frame in flight). The compute shader reads each entity's bind-pose vertices
 * + skin attributes from the static / sparse skin region of the mega buffer and
 * writes the post-skin 48 B vertex + 12 B position outputs into that slot.
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
}

namespace ve {

class VENGINE_API SkinningPrePass {
public:
	SkinningPrePass(VeDevice& device, VeDescriptorPool& descriptor_pool,
	                std::filesystem::path shader_path, EventBus& event_bus);
	~SkinningPrePass();

	SkinningPrePass(const SkinningPrePass&) = delete;
	SkinningPrePass& operator=(const SkinningPrePass&) = delete;

	void updatePalette(Registry& registry, uint32_t frame_index);

	void dispatch(VeFrameInfo& fi, PbrMegaBuffer& mega_buffer);

	// Returns the absolute offset (in vertices) into the mega VBO + mega shadow VBO
	// where this entity's post-skin output sits for the given frame, or UINT32_MAX
	// if the entity has no active slot 
	static constexpr uint32_t INVALID_OFFSET = UINT32_MAX;
	uint32_t getSkinnedVertexOffset(Entity entity, uint32_t frame_index,
	                                 const PbrMegaBuffer& mega_buffer) const;

private:
	struct SubspaceSlot {
		uint32_t offset;        // in [0, MAX_SKINNED_VERTICES_PER_FRAME)
		uint32_t vertex_count;
	};

	struct SkinDispatch {
		Entity entity;
		uint32_t vertex_count;
		uint32_t palette_offset; // in matrices
	};

	void createSetLayout();
	void createPipelineLayout();
	void createPipeline();
	void subscribeToRegistry(Registry& registry);
	void refreshDescriptors(uint32_t frame_index, PbrMegaBuffer& mega_buffer);

	bool allocateSlotForEntity(Entity entity, VeMesh* mesh);
	void freeSlotForEntity(Entity entity);
	void resetAllocator();

	VeDevice& m_ve_device;
	VeDescriptorPool& m_descriptor_pool;
	std::filesystem::path m_shader_path;
	Registry* m_registry = nullptr;

	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_palette_ssbos;
	std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_palette_count{}; // matrices currently used per frame

	std::unique_ptr<VeDescriptorSetLayout> m_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;

	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptor_sets{
		vk::raii::DescriptorSet{nullptr}, vk::raii::DescriptorSet{nullptr}};
	std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> m_cached_mega_vbo{};
	std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> m_cached_mega_skin_vbo{};
	std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> m_cached_mega_shadow_vbo{};

	// Slot allocator (works in the per-frame subspace [0, MAX_SKINNED_VERTICES_PER_FRAME)).
	std::unordered_map<uint32_t, SubspaceSlot> m_slots; // keyed by entity.index()
	std::vector<SubspaceSlot> m_free_ranges;            // sorted by offset, disjoint

	// Per-frame dispatch list, populated by updatePalette and consumed by dispatch.
	std::array<std::vector<SkinDispatch>, MAX_FRAMES_IN_FLIGHT> m_pending_dispatches;
};

} // namespace ve