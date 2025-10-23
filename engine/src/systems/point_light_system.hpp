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

class VENGINE_API PointLightSystem {
public:
	PointLightSystem( 
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& material_set_layout,
		vk::Format color_format,
		std::filesystem::path shader_path);
	~PointLightSystem();

	//destroy copy and move constructors and assignment operators
	PointLightSystem(const PointLightSystem&) = delete;
	PointLightSystem& operator=(const PointLightSystem&) = delete;

	void update(VeFrameInfo& frame_info, UniformBufferObject& ubo);
	void render(VeFrameInfo& frame_info) const;

private:
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout);
	void createPipeline(vk::Format color_format);

	VeDevice& m_ve_device;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
	std::filesystem::path m_shader_path;
};
}

