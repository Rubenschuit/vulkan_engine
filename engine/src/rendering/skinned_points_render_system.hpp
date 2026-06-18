/* debug visualization for DeformPrePass output.
 *
 * Iterates skinned drawables and draws each entity's joints
 * as a colored point cloud.
 */
#pragma once
#include "ve_export.hpp"
#include "rendering/ve_frame_info.hpp"

#include <filesystem>
#include <memory>

namespace ve {
	class VeDevice;
	class VePipeline;
	class DeformPrePass;
	class EventBus;
	class PbrMegaBuffer;
}

namespace ve {

class VENGINE_API SkinnedPointsRenderSystem {
public:
	SkinnedPointsRenderSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path,
		EventBus& event_bus);
	~SkinnedPointsRenderSystem();

	SkinnedPointsRenderSystem(const SkinnedPointsRenderSystem&) = delete;
	SkinnedPointsRenderSystem& operator=(const SkinnedPointsRenderSystem&) = delete;

	void render(VeFrameInfo& fi, const DeformPrePass& prepass, const PbrMegaBuffer& mega_buffer);
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count);

	void setPointSize(float px) { m_point_size = px; }

private:
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout);
	void createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_pipeline;
	float m_point_size = 6.0f;
};

} // namespace ve