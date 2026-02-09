#pragma once
#include "ve_export.hpp"
#include "rendering/ve_frame_info.hpp"

#include <memory>
#include <filesystem>

namespace ve {

class VeDevice;
class VePipeline;
class VeBuffer;

class VENGINE_API AabbDebugRenderSystem {
public:
	AabbDebugRenderSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path);
	~AabbDebugRenderSystem();

	AabbDebugRenderSystem(const AabbDebugRenderSystem&) = delete;
	AabbDebugRenderSystem& operator=(const AabbDebugRenderSystem&) = delete;

	void render(VeFrameInfo& frame_info) const;
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count);

private:
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout);
	void createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count);
	void createVertexBuffer();

	static constexpr uint32_t MAX_AABB_BOXES = 2000;
	static constexpr uint32_t VERTICES_PER_BOX = 24; // 12 edges * 2 vertices

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
	mutable std::unique_ptr<VeBuffer> m_vertex_buffer;
	vk::Format m_color_format = vk::Format::eUndefined;
	vk::SampleCountFlagBits m_sample_count = vk::SampleCountFlagBits::e1;
};

} // namespace ve
