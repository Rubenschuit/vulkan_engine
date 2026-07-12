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

class VENGINE_API GeometryPrePassSystem {
public:
	GeometryPrePassSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& bindless_set_layout,
		vk::SampleCountFlagBits sample_count,
		vk::Format normal_roughness_format,
		std::filesystem::path shader_path,
		EventBus& event_bus);
	~GeometryPrePassSystem();

	GeometryPrePassSystem(const GeometryPrePassSystem&) = delete;
	GeometryPrePassSystem& operator=(const GeometryPrePassSystem&) = delete;

	// Render depth using indirect draw commands from PbrRenderSystem.
	// Must be called AFTER PbrRenderSystem::prepareFrame().
	void render(VeFrameInfo& frame_info,
	            PbrMegaBuffer& mega_buffer,
	            const vk::raii::DescriptorSet& bindless_set,
	            const VeBuffer& indirect_buffer,
	            const uint32_t* bucket_offsets,
	            const uint32_t* bucket_counts,
	            uint32_t bucket_count) const;

	void renderGpuCulled(VeFrameInfo& frame_info,
	                     PbrMegaBuffer& mega_buffer,
	                     const vk::raii::DescriptorSet& bindless_set,
	                     const VeBuffer& indirect_buffer,
	                     const uint32_t* bucket_group_offsets,
	                     const uint32_t* bucket_group_counts,
	                     uint32_t bucket_count,
	                     const VeBuffer* compacted_buffer = nullptr,
	                     const VeBuffer* compact_count_buffer = nullptr,
	                     const vk::raii::DescriptorSet* global_set_override = nullptr) const;
	void renderGpuCulledMeshlets(VeFrameInfo& frame_info,
	                              PbrMegaBuffer& mega_buffer,
	                              const vk::raii::DescriptorSet& bindless_set,
	                              const VeBuffer& meshlet_indirect, const VeBuffer& draw_counts,
	                              const uint32_t* cpu_draw_counts = nullptr,
	                              const vk::raii::DescriptorSet* global_set_override = nullptr) const;

	void recreatePipeline(vk::SampleCountFlagBits sample_count);

private:
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout,
	                          const vk::raii::DescriptorSetLayout& bindless_set_layout);
	void createPipeline(vk::SampleCountFlagBits sample_count);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::Format m_normal_roughness_format;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
	std::unique_ptr<VePipeline> m_masked_pipeline; // ALPHA_MASK_SPEC=1, buckets 2-3
};

} // namespace ve
