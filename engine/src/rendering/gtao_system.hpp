/*
 * GTAOSystem: Ground Truth Ambient Occlusion implementation.
 * Based on "Practical Realtime Strategies for Accurate Indirect Occlusion" by Jimenez, et al. https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf
 * Computes screen-space ambient occlusion in a compute shader, with an optional bilateral blur pass.
 * Outputs a fullscreen AO texture for sampling in the main render pass.
*/

#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"

#include <memory>
#include <array>
#include <filesystem>

namespace ve {
	class VeDevice;
	class VeImage;
	class VeBuffer;
	class VeDescriptorPool;
	class VeDescriptorSetLayout;
	class EventBus;
}

namespace ve {

class VENGINE_API GtaoSystem {
public:
	GtaoSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		VeResourceManager& resource_manager,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		std::filesystem::path shader_path,
		vk::Extent2D ao_extent,
		vk::Extent2D depth_extent,
		const vk::raii::ImageView& depth_image_view,
		vk::Image depth_image,
		EventBus& event_bus);
	~GtaoSystem();

	GtaoSystem(const GtaoSystem&) = delete;
	GtaoSystem& operator=(const GtaoSystem&) = delete;

	// Record GTAO compute + bilateral blur on the GRAPHICS command buffer.
	// Must be called AFTER depth pre-pass, BEFORE beginSceneRender.
	void dispatch(VeFrameInfo& frame_info);

	// Recreate AO images when swapchain resizes or resolution toggle changes.
	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D ao_extent,
		vk::Extent2D depth_extent,
		const vk::raii::ImageView& depth_image_view, vk::Image depth_image);

	// Descriptor set layout for Set 5 (AO output for PBR/simple shaders)
	const vk::raii::DescriptorSetLayout& getAoSetLayout() const {
		return m_output_set_layout->getDescriptorSetLayout();
	}

	// Per-frame output descriptor set for fragment shader binding
	vk::raii::DescriptorSet& getOutputDescriptorSet(uint32_t frame_index) {
		return m_output_descriptor_sets[frame_index];
	}

	// Dummy white descriptor set (AO=1.0 everywhere) when GTAO is disabled
	vk::raii::DescriptorSet& getDummyOutputDescriptorSet() {
		return m_dummy_output_descriptor_set;
	}

private:
	void createAoImages(vk::Extent2D extent);
	void createComputeSetLayout();
	void createBlurSetLayout();
	void createOutputSetLayout();
	void createSampler();
	void createGtaoPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout);
	void createBlurPipelineLayout();
	void createPipelines();
	void createDescriptorSets(VeDescriptorPool& descriptor_pool);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::Extent2D m_extent{};         // AO resolution (may be half-res)
	vk::Extent2D m_depth_extent{};   // depth buffer resolution

	float m_radius = 0.5f;
	float m_intensity = 1.5f;

	// Per-frame AO images: raw GTAO output + blur intermediate.
	// After H+V blur, final result lives in m_ao_raw_images[frame].
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_ao_raw_images;
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_ao_blur_images;

	// Default white texture for disabled state
	ResourceHandle<VeTexture> m_default_ao_texture;

	// Sampler for output reads (linear clamp)
	vk::raii::Sampler m_linear_clamp_sampler{nullptr};

	// Descriptor set layouts
	std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;  // GTAO I/O: depth + AO storage
	std::unique_ptr<VeDescriptorSetLayout> m_blur_set_layout;     // Blur I/O: depth + AO in + AO out
	std::unique_ptr<VeDescriptorSetLayout> m_output_set_layout;   // Set 5: AO sampled + sampler

	// Pipelines
	vk::raii::PipelineLayout m_gtao_pipeline_layout{nullptr};
	vk::raii::PipelineLayout m_blur_pipeline_layout{nullptr};
	vk::raii::Pipeline m_gtao_pipeline{nullptr};
	vk::raii::Pipeline m_blur_pipeline{nullptr};

	// Per-frame descriptor sets
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_compute_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_blur_h_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_blur_v_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_output_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	vk::raii::DescriptorSet m_dummy_output_descriptor_set{nullptr};

	// Cached depth image for barriers + descriptors (single-sample resolved depth)
	vk::Image m_depth_image{};
	vk::ImageView m_depth_image_view{};

	// Shader modules
	vk::raii::ShaderModule m_gtao_module{nullptr};
	vk::raii::ShaderModule m_blur_module{nullptr};
};

} // namespace ve
