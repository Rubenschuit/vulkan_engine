/* Class for rendering simple 3D coordinate axes */
#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "game/ve_frame_info.hpp"
#include "core/ve_resource_manager.hpp"

#include <memory>
#include <filesystem>

namespace ve {
    // Forward declarations
    class VeDevice;
    class VePipeline;
    class VeMesh;
    class VeResourceManager;
}

namespace ve {

class VENGINE_API AxesRenderSystem {
public:

AxesRenderSystem(
		VeDevice& device,
		VeResourceManager& resource_manager,
		const vk::raii::DescriptorSetLayout& descriptor_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path);
	~AxesRenderSystem();

	AxesRenderSystem(const AxesRenderSystem&) = delete;
	AxesRenderSystem& operator=(const AxesRenderSystem&) = delete;

	void render(VeFrameInfo& frame_info) const;
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
		m_ve_pipeline.reset();
		createPipeline(color_format, sample_count);
	}

private:
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& descriptor_set_layout);
	void createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);
	void createAxesModel();

	VeDevice& m_ve_device;
	VeResourceManager* m_resource_manager = nullptr;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
	ResourceHandle<VeMesh> m_axes_mesh;
	std::filesystem::path m_shader_path;
};

}
