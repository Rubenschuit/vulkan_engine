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

	// Render depth for all opaque groups. Must be called AFTER PbrRenderSystem::prepareFrame()
	// and between beginDepthPrePass/endDepthPrePass.
	void render(VeFrameInfo& frame_info,
	            const std::vector<PbrRenderSystem::InstanceGroup>& opaque_groups) const;

	/// Record a range of opaque groups [begin_idx, end_idx) for depth-only rendering.
	/// The CB must already be recording. Binds pipeline, descriptor set, and iterates groups.
	void recordRange(vk::raii::CommandBuffer& cmd, VeFrameInfo& frame_info,
	                 const std::vector<PbrRenderSystem::InstanceGroup>& opaque_groups,
	                 uint32_t begin_idx, uint32_t end_idx) const;

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
