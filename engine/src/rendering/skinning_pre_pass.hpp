/* Compute pre-pass that produces post-skin vertex streams.
 *
 * For each entity with MeshComponent + SkinComponent, allocates two ring-buffered
 * output buffers (48B full vertex + 12B position-only) and dispatches a compute
 * shader that reads the input vertex stream + skin attributes + joint palette
 * and writes both outputs in one pass. Output buffers are usable directly as
 * vertex buffers by downstream graphics passes
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

	void dispatch(VeFrameInfo& fi);

	VeBuffer* getOutputFullBuffer(Entity entity, uint32_t frame_index) const;
	VeBuffer* getOutputPositionBuffer(Entity entity, uint32_t frame_index) const;

private:
	struct InstanceFrame {
		std::unique_ptr<VeBuffer> out_full;     // 48B/vertex, STORAGE+VERTEX
		std::unique_ptr<VeBuffer> out_pos;      // 12B/vertex, STORAGE+VERTEX
		uint32_t vertex_count = 0;
		VkBuffer input_vertex_buf = VK_NULL_HANDLE;
		VkBuffer input_skin_buf = VK_NULL_HANDLE;
		vk::raii::DescriptorSet descriptor_set{nullptr};
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
	void freeEntityOutputs(Entity entity);
	uint64_t makeKey(Entity entity, uint32_t frame_index) const {
		return (static_cast<uint64_t>(entity.id()) << 32) | frame_index;
	}
	InstanceFrame& getOrAllocateOutputs(Entity entity, uint32_t frame_index,
	                                    uint32_t vertex_count,
	                                    VeBuffer& input_vertex_buf, VeBuffer& input_skin_buf);
	void writeDescriptor(InstanceFrame& inst, uint32_t frame_index,
	                     VeBuffer& input_vertex_buf, VeBuffer& input_skin_buf);

	VeDevice& m_ve_device;
	VeDescriptorPool& m_descriptor_pool;
	std::filesystem::path m_shader_path;

	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_palette_ssbos;
	std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_palette_count{}; // matrices currently used per frame

	std::unique_ptr<VeDescriptorSetLayout> m_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;

	std::unordered_map<uint64_t, InstanceFrame> m_instance_outputs;

	// Per-frame dispatch list, populated by updatePalette and consumed by dispatch.
	std::array<std::vector<SkinDispatch>, MAX_FRAMES_IN_FLIGHT> m_pending_dispatches;
};

} // namespace ve