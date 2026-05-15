#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"

#include <array>
#include <memory>
#include <vector>
#include <filesystem>

namespace ve {
    // Forward declarations
    class VeDevice;
    class VePipeline;
    class EventBus;
    class VeDescriptorPool;
    class VeDescriptorSetLayout;
}

namespace ve {

class VENGINE_API LightSystem {
public:
	LightSystem(
		VeDevice& device,
		VeResourceManager& resource_manager,
		VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path,
		EventBus& event_bus);
	~LightSystem();

	//destroy copy and move constructors and assignment operators
	LightSystem(const LightSystem&) = delete;
	LightSystem& operator=(const LightSystem&) = delete;

	void updateUniformBuffer(VeFrameInfo& frame_info, UniformBufferObject& ubo);
	void render(VeFrameInfo& frame_info) const;
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
		m_ve_pipeline.reset();
		createPipeline(color_format, sample_count);
	}

private:
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& billboard_set_layout);
	void createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);
	void createBillboardDescriptorSet(VeResourceManager& resource_manager, VeDescriptorPool& descriptor_pool);

	VeDevice& m_ve_device;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
	std::filesystem::path m_shader_path;

	std::unique_ptr<VeDescriptorSetLayout> m_billboard_set_layout;
	ResourceHandle<VeTexture> m_particle_handle;
	vk::raii::DescriptorSet m_billboard_descriptor_set{nullptr};

	// Per-cascade Z-snap hysteresis state
	struct CascadeZState {
		float snapped_z = 0.0f;
		float z_snap = 0.0f;
		glm::vec3 cached_eye{0.0f};
		bool valid = false;
	};
	std::array<CascadeZState, NUM_CSM_CASCADES> m_cascade_z_state{};
};
}
