#pragma once
#include "ve_export.hpp"
#include "rendering/ve_frame_info.hpp"
#include "rendering/pbr_render_system.hpp"

#include <memory>
#include <vector>
#include <filesystem>

namespace ve {
	class VeDevice;
	class VePipeline;
	class VeBuffer;
	class PbrMegaBuffer;
	class EventBus;
}

namespace ve {

class VENGINE_API DepthPrePassSystem {
public:
	DepthPrePassSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path,
		EventBus& event_bus);
	~DepthPrePassSystem();

	DepthPrePassSystem(const DepthPrePassSystem&) = delete;
	DepthPrePassSystem& operator=(const DepthPrePassSystem&) = delete;

	// Render depth using indirect draw commands from PbrRenderSystem.
	// Must be called AFTER PbrRenderSystem::prepareFrame().
	void render(VeFrameInfo& frame_info,
	            PbrMegaBuffer& mega_buffer,
	            const VeBuffer& indirect_buffer,
	            const uint32_t* bucket_offsets,
	            const uint32_t* bucket_counts,
	            uint32_t bucket_count) const;

	void renderGpuCulled(VeFrameInfo& frame_info,
	                     PbrMegaBuffer& mega_buffer,
	                     const VeBuffer& indirect_buffer,
	                     const uint32_t* bucket_group_offsets,
	                     const uint32_t* bucket_group_counts,
	                     uint32_t bucket_count,
	                     const VeBuffer* compacted_buffer = nullptr,
	                     const VeBuffer* compact_count_buffer = nullptr,
	                     const vk::raii::DescriptorSet* global_set_override = nullptr) const;
	void renderGpuCulledMeshlets(VeFrameInfo& frame_info,
	                              PbrMegaBuffer& mega_buffer,
	                              const VeBuffer& meshlet_indirect, const VeBuffer& draw_counts,
	                              const uint32_t* cpu_draw_counts = nullptr,
	                              const vk::raii::DescriptorSet* global_set_override = nullptr) const;

	// Per-instance depth prepass for skinned meshes.
	void renderSkinned(VeFrameInfo& frame_info,
	                   const std::vector<PbrRenderSystem::Drawable>& skinned_drawables) const;

	void recreatePipeline(vk::SampleCountFlagBits sample_count);

private:
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout);
	void createPipeline(vk::SampleCountFlagBits sample_count);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
};

} // namespace ve
