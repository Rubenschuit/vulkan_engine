/* This class manages the creation and configuration of Vulkan pipelines.
   It also creates the necessary shader modules. */
# pragma once

#include <vulkan/vulkan_raii.hpp>

namespace ve { class VeDevice; }

#include <string>
#include <vector>

namespace ve {
	struct PipelineConfigInfo {
		PipelineConfigInfo(const PipelineConfigInfo&) = delete;
		PipelineConfigInfo& operator=(const PipelineConfigInfo&) = delete;
		PipelineConfigInfo() = default;

		std::vector<vk::DynamicState> dynamic_state_enables{};
		vk::PipelineDynamicStateCreateInfo dynamic_state_info{};
		vk::PipelineInputAssemblyStateCreateInfo input_assembly_info{};
		vk::PipelineRasterizationStateCreateInfo rasterization_info{};
		vk::PipelineMultisampleStateCreateInfo multisample_info{};
		vk::PipelineDepthStencilStateCreateInfo depth_stencil_info{};
		vk::PipelineViewportStateCreateInfo viewport_info{};
		vk::PipelineColorBlendAttachmentState color_blend_attachment{};
		vk::PipelineColorBlendStateCreateInfo color_blend_info{};
		vk::PipelineLayout pipeline_layout = nullptr;
		vk::RenderPass render_pass = nullptr;
		uint32_t subpass = 0;
		vk::Format color_format = vk::Format::eUndefined;
		//vk::Format depth_format = vk::Format::eUndefined;
	};

	class VePipeline {
	public:
		VePipeline(
			VeDevice&ve_device,
			const char* shader_file_path,
			const PipelineConfigInfo& config_info);

		~VePipeline();

		// Prevent copying
		VePipeline(const VePipeline&) = delete;
		VePipeline& operator=(const VePipeline&) = delete;

		vk::Pipeline getPipeline() { return *graphics_pipeline; }
		static void defaultPipelineConfigInfo(PipelineConfigInfo& config_info);

	private:
		static std::vector<char> readFile(const char* file_path);

		void createGraphicsPipeline(
			const char* shader_file_path,
			const PipelineConfigInfo& config_info);

		void createShaderModule(const std::vector<char>& code, vk::raii::ShaderModule* shader_module);

		VeDevice& ve_device; // will outlive the pipeline class
		vk::raii::Pipeline graphics_pipeline{nullptr};
		vk::raii::ShaderModule shader_module{nullptr};
	};
}