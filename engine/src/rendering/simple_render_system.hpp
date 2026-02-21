#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"

#include <array>
#include <memory>
#include <vector>
#include <filesystem>

namespace ve {
    // Forward declarations
    class VeDevice;
    class VePipeline;
    class VeMesh;
    class MeshComponent;
}

namespace ve {

class VENGINE_API SimpleRenderSystem {
public:
	SimpleRenderSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& material_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_mask_set_layout,
		const vk::raii::DescriptorSetLayout& cluster_set_layout,
		const vk::raii::DescriptorSetLayout& ao_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path);
	~SimpleRenderSystem();

	//destroy copy and move constructors and assignment operators
	SimpleRenderSystem(const SimpleRenderSystem&) = delete;
	SimpleRenderSystem& operator=(const SimpleRenderSystem&) = delete;

	void renderObjects(VeFrameInfo& frame_info) const;
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count ) {
		for (auto& p : m_pipelines) 
			p.reset();
		for (auto& p : m_pipelines_mask) 
			p.reset();
		VE_LOGI("SimpleRenderSystem::recreatePipeline with topology: " << (m_topology == vk::PrimitiveTopology::eTriangleList ? "Triangle List" : "Line List") << ", and sample count: " << static_cast<int>(sample_count));
		createPipelines(color_format, sample_count);
	}
	void setTopology(vk::PrimitiveTopology topology) {
		m_topology = topology;
	}
	void setShadowSamples(uint32_t pcf_samples, uint32_t pcss_filter_samples) {
		m_pcf_samples = pcf_samples;
		m_pcss_filter_samples = pcss_filter_samples;
		for (auto& p : m_pipelines) 
			p.reset();
		for (auto& p : m_pipelines_mask) 
			p.reset();
		createPipelines(m_color_format, m_sample_count);
	}

private:
	void createPipelineLayout(
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& material_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_mask_set_layout,
		const vk::raii::DescriptorSetLayout& cluster_set_layout,
		const vk::raii::DescriptorSetLayout& ao_set_layout);
	void createPipelines(vk::Format color_format, vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);

	uint32_t m_pcf_samples = 8;
	uint32_t m_pcss_filter_samples = 16;
	static constexpr uint32_t SHADOW_MODE_COUNT = 4;  // DISABLED, REGULAR, PCF, PCSS
	VeDevice& m_ve_device;

	std::filesystem::path m_shader_path;
	vk::PrimitiveTopology m_topology = vk::PrimitiveTopology::eTriangleList;
	vk::Format m_color_format = vk::Format::eUndefined;
	vk::SampleCountFlagBits m_sample_count = vk::SampleCountFlagBits::e1;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::array<std::unique_ptr<VePipeline>, SHADOW_MODE_COUNT> m_pipelines;      // mask off
	std::array<std::unique_ptr<VePipeline>, SHADOW_MODE_COUNT> m_pipelines_mask; // mask on

	// Instanced draw groups
	struct InstanceGroup {
		VeMesh* mesh = nullptr;
		VkDescriptorSet material_set = VK_NULL_HANDLE;
		uint32_t first_instance = 0;
		uint32_t instance_count = 0;
		float has_texture = 0.0f;
	};
	mutable std::vector<InstanceGroup> m_instance_groups;
};
}

