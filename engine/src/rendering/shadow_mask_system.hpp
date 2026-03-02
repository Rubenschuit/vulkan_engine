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
	class VeDescriptorPool;
	class VeDescriptorSetLayout;
}

namespace ve {

class VENGINE_API ShadowMaskSystem {
public:
	ShadowMaskSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		VeResourceManager& resource_manager,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout,
		std::filesystem::path shader_path,
		vk::Extent2D mask_extent,
		vk::Extent2D depth_extent,
		const vk::raii::ImageView& depth_image_view,
		const vk::raii::Image& depth_image);
	~ShadowMaskSystem();

	ShadowMaskSystem(const ShadowMaskSystem&) = delete;
	ShadowMaskSystem& operator=(const ShadowMaskSystem&) = delete;

	/// Record compute dispatch on the graphics command buffer.
	/// Depth must already be in eDepthStencilReadOnlyOptimal (caller handles).
	void dispatch(VeFrameInfo& frame_info);

	/// Recreate shadow mask image for swapchain resize or resolution change
	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D mask_extent,
		vk::Extent2D depth_extent,
		const vk::raii::ImageView& depth_image_view, const vk::raii::Image& depth_image);

	/// Descriptor set layout for Set 3 (shadow mask output for PBR/simple)
	const vk::raii::DescriptorSetLayout& getShadowMaskSetLayout() const {
		return m_output_set_layout->getDescriptorSetLayout();
	}

	/// Per-frame output descriptor set for PBR/simple binding
	vk::raii::DescriptorSet& getOutputDescriptorSet(uint32_t frame_index) {
		return m_output_descriptor_sets[frame_index];
	}

	/// Dummy output descriptor set (white texture) for when shadow mask is disabled
	vk::raii::DescriptorSet& getDummyOutputDescriptorSet() {
		return m_dummy_output_descriptor_set;
	}

	void setShadowSamples(uint32_t pcf_samples, uint32_t pcss_filter_samples);

private:
	void createShadowMaskImage(vk::Extent2D extent);
	void createComputeSetLayout();
	void createOutputSetLayout();
	void createSampler();
	void createPipelineLayout(
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout);
	void createPipelines();
	void createDescriptorSets(VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& global_set_layout);
	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::Extent2D m_extent{};       // shadow mask resolution (may be half-res)
	vk::Extent2D m_depth_extent{}; // depth buffer resolution (always full screen)
	uint32_t m_pcf_samples = 8;
	uint32_t m_pcss_filter_samples = 16;

	// Shadow mask image (R8_UNORM, screen resolution)
	std::unique_ptr<VeImage> m_shadow_mask_image;
	// Default white texture for when shadow mask is disabled
	ResourceHandle<VeTexture> m_default_mask_texture;

	// Sampler for output reads (linear clamp)
	vk::raii::Sampler m_linear_clamp_sampler{nullptr};

	// Descriptor set layouts
	std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;  // Set 1: depth + mask storage
	std::unique_ptr<VeDescriptorSetLayout> m_output_set_layout;   // Set 3: mask sampled + sampler

	// Pipeline (one per shadow mode variant, always single-sample depth)
	static constexpr uint32_t SHADOW_MODE_COUNT = 4;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::array<vk::raii::Pipeline, SHADOW_MODE_COUNT> m_pipelines{
		vk::raii::Pipeline{nullptr}, vk::raii::Pipeline{nullptr},
		vk::raii::Pipeline{nullptr}, vk::raii::Pipeline{nullptr}};

	// Per-frame descriptor sets
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_compute_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_output_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();

	vk::raii::DescriptorSet m_dummy_output_descriptor_set{nullptr}; // for when shadow mask is disabled

	// Cached depth image (single-sample resolved depth) for descriptor
	vk::Image m_depth_image{};
	vk::ImageView m_depth_image_view{};

	// SPIR-V shader module
	vk::raii::ShaderModule m_shader_module{nullptr};
};

} // namespace ve
