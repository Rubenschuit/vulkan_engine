/* SkyboxRenderSystem is responsible for rendering a skybox.
It creates a pipeline, loads a cube model and renders it.
Size is hardcoded in .cpp for now.*/

#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "game/ve_frame_info.hpp"

#include <memory>
#include <vector>
#include <filesystem>

namespace ve {
    // Forward declarations
    class VeDevice;
    class VePipeline;
}

namespace ve {

class VENGINE_API SkyboxRenderSystem {
public:
	SkyboxRenderSystem( VeDevice& device,
						const vk::raii::DescriptorSetLayout& global_set_layout,
						const vk::raii::DescriptorSetLayout& material_set_layout,
						vk::Format color_format,
						vk::SampleCountFlagBits sample_count,
						std::filesystem::path shader_path,
						const std::filesystem::path& cube_model_path);
	~SkyboxRenderSystem();

	//destroy copy and move constructors and assignment operators
	SkyboxRenderSystem(const SkyboxRenderSystem&) = delete;
	SkyboxRenderSystem& operator=(const SkyboxRenderSystem&) = delete;

	// Slowly rotate the skybox over time and render it
	void render(VeFrameInfo& frame_info);
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
		m_ve_pipeline.reset();
		createPipeline(color_format, sample_count);
	}

private:
	void loadCubeModel(const std::filesystem::path& cube_model_path);
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout);
	void createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);

	VeDevice& m_ve_device;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
	VeGameObject m_cube_object = VeGameObject::createGameObject();
	std::filesystem::path  m_shader_path;
};
}

