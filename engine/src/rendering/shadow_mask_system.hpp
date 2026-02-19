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
		vk::SampleCountFlagBits depth_sample_count,
		const vk::raii::ImageView& depth_image_view,
		const vk::raii::Image& depth_image);
	~ShadowMaskSystem();

	ShadowMaskSystem(const ShadowMaskSystem&) = delete;
	ShadowMaskSystem& operator=(const ShadowMaskSystem&) = delete;

	/// Save the current frame's UBO data. Must be called every frame (even when
	/// the shadow mask is inactive) so that the previous frame's data is available
	/// when the mask is dispatched.  The data is written to the per-frame compute
	/// UBO one frame later so that the compute shader reads matrices consistent
	/// with the previous frame's depth buffer and shadow maps.
	void savePrevFrameUBO(const UniformBufferObject& ubo, uint32_t current_frame);

	/// Record compute dispatch on the compute command buffer.
	/// savePrevFrameUBO() must have been called at least once before the first dispatch.
	void dispatch(VeFrameInfo& frame_info);

	/// Recreate shadow mask image for swapchain resize or resolution change
	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D mask_extent,
		vk::Extent2D depth_extent, vk::SampleCountFlagBits depth_sample_count,
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

	/// Whether the MSAA compute path is available (requires shaderStorageImageMultisample)
	bool hasMsaaSupport() const { return m_has_ms_support; }

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
	void createComputeUBOs(VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& global_set_layout);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::Extent2D m_extent{};       // shadow mask resolution (may be half-res)
	vk::Extent2D m_depth_extent{}; // depth buffer resolution (always full screen)
	vk::SampleCountFlagBits m_depth_sample_count;
	uint32_t m_pcf_samples = 8;
	uint32_t m_pcss_filter_samples = 16;
	bool m_has_ms_support = false;

	// Shadow mask image (R8_UNORM, screen resolution)
	std::unique_ptr<VeImage> m_shadow_mask_image;
	// Default white texture for when shadow mask is disabled
	ResourceHandle<VeTexture> m_default_mask_texture;

	// Sampler for output reads (linear clamp)
	vk::raii::Sampler m_linear_clamp_sampler{nullptr};

	// Descriptor set layouts
	std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;  // Set 1: depth + mask storage
	std::unique_ptr<VeDescriptorSetLayout> m_output_set_layout;   // Set 3: mask sampled + sampler

	// Pipeline (one set of 4 shadow-mode variants per depth type: non-MSAA + MSAA)
	static constexpr uint32_t SHADOW_MODE_COUNT = 4;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::array<vk::raii::Pipeline, SHADOW_MODE_COUNT> m_pipelines{
		vk::raii::Pipeline{nullptr}, vk::raii::Pipeline{nullptr},
		vk::raii::Pipeline{nullptr}, vk::raii::Pipeline{nullptr}};
	std::array<vk::raii::Pipeline, SHADOW_MODE_COUNT> m_pipelines_ms{
		vk::raii::Pipeline{nullptr}, vk::raii::Pipeline{nullptr},
		vk::raii::Pipeline{nullptr}, vk::raii::Pipeline{nullptr}};

	// Per-frame descriptor sets
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_compute_descriptor_sets{
		vk::raii::DescriptorSet{nullptr}, vk::raii::DescriptorSet{nullptr}};
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_output_descriptor_sets{
		vk::raii::DescriptorSet{nullptr}, vk::raii::DescriptorSet{nullptr}};

	vk::raii::DescriptorSet m_dummy_output_descriptor_set{nullptr}; // for when shadow mask is disabled

	// Per-frame compute UBO: stores previous frame's matrices so the compute
	// shader reads data consistent with the previous frame's depth and shadow maps.
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_compute_ubos;
	std::unique_ptr<VeBuffer> m_dummy_instance_buffer; // binding 1 (unused by compute)
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_compute_global_descriptor_sets{
		vk::raii::DescriptorSet{nullptr}, vk::raii::DescriptorSet{nullptr}};
	UniformBufferObject m_prev_ubo_data{};
	bool m_has_prev_data = false;

	// Cached depth image (from swapchain depth buffer) for barriers + descriptor
	vk::Image m_depth_image{};
	vk::ImageView m_depth_image_view{};

	// SPIR-V shader modules (kept alive for pipeline recreation)
	vk::raii::ShaderModule m_shader_module{nullptr};
	vk::raii::ShaderModule m_shader_module_ms{nullptr};
};

} // namespace ve
