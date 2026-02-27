#pragma once
#include "ve_export.hpp"
#include "rendering/ve_frame_info.hpp"

#include <memory>
#include <vector>
#include <filesystem>

namespace ve {
	class VeDevice;
	class VePipeline;
	class VeBuffer;
	class PbrMegaBuffer;
}

namespace ve {

class VENGINE_API DepthPrePassSystem {
public:
	DepthPrePassSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path);
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
	                     const VeBuffer& count_buffer,
	                     uint32_t bucket_stride,
	                     uint32_t max_draw_count,
	                     uint32_t bucket_count,
	                     bool use_draw_count) const;

	void recreatePipeline(vk::SampleCountFlagBits sample_count) {
		m_ve_pipeline.reset();
		createPipeline(sample_count);
	}

private:
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout);
	void createPipeline(vk::SampleCountFlagBits sample_count);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
};

} // namespace ve
