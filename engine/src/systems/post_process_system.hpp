#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "core/ve_device.hpp"
#include "core/ve_pipeline.hpp"
#include "core/ve_descriptors.hpp"
#include "game/ve_frame_info.hpp"

#include <memory>
#include <filesystem>

namespace ve {

class VENGINE_API PostProcessSystem {
public:
	PostProcessSystem(
		VeDevice& device,
		vk::Format color_format,
		const vk::raii::ImageView& resolve_target_view,
		std::filesystem::path shader_path);
	~PostProcessSystem();

	PostProcessSystem(const PostProcessSystem&) = delete;
	PostProcessSystem& operator=(const PostProcessSystem&) = delete;

	void render(vk::raii::CommandBuffer& command_buffer, const PostProcessPushConstant& push);
	void recreatePipeline(vk::Format color_format, const vk::raii::ImageView& resolve_target_view);

private:
	void createDescriptorSetLayout();
	void createDescriptorPool();
	void createDescriptorSet(const vk::raii::ImageView& resolve_target_view);
	void createPipelineLayout();
	void createPipeline(vk::Format color_format);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;

	std::unique_ptr<VeDescriptorSetLayout> m_descriptor_set_layout;
	std::unique_ptr<VeDescriptorPool> m_descriptor_pool;
	vk::raii::DescriptorSet m_descriptor_set{nullptr};
	std::unique_ptr<vk::raii::Sampler> m_sampler;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
};

} // namespace ve

