#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"

#include <memory>
#include <array>
#include <vector>

namespace ve {
class VeDevice;
class VeImage;
class VeDescriptorPool;
class VeDescriptorSetLayout;
class VeComputePipeline;
class EventBus;
}

namespace ve {

class VENGINE_API HizSystem {
public:
	HizSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		vk::Extent2D extent,
		const vk::raii::ImageView& depth_image_view,
		vk::Image depth_image,
		const std::filesystem::path& shaders_dir,
		EventBus& event_bus);
	~HizSystem();

	HizSystem(const HizSystem&) = delete;
	HizSystem& operator=(const HizSystem&) = delete;

	// Generate the Hi-Z mip chain from the current depth buffer.
	// Depth must be in eDepthStencilReadOnlyOptimal before calling.
	// After return, depth is still in eDepthStencilReadOnlyOptimal
	// and Hi-Z is in eShaderReadOnlyOptimal (all mips).
	void generate(vk::raii::CommandBuffer& cmd, uint32_t frame_index);

	// Recreate images on swapchain resize.
	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D extent,
	              const vk::raii::ImageView& depth_image_view,
	              vk::Image depth_image);

	const vk::raii::ImageView& getHizImageView(uint32_t frame) const;
	vk::Image getHizImage(uint32_t frame) const;
	uint32_t getMipLevels() const { return m_mip_levels; }
	uint32_t getWidth() const { return m_width; }
	uint32_t getHeight() const { return m_height; }
	uint32_t getScreenWidth() const { return m_screen_width; }
	uint32_t getScreenHeight() const { return m_screen_height; }
	const vk::raii::Sampler& getSampler() const { return m_nearest_sampler; }

private:
	void createHizImages(vk::Extent2D extent);
	void createMipViews();
	void createSampler();
	void createDummyImage();
	void createComputeSetLayout();
	void createPipelineLayout();
	void createPipeline(const std::filesystem::path& shaders_dir);
	void createDescriptorSets(VeDescriptorPool& pool);

	VeDevice& m_ve_device;

	// m_width/m_height are the padded (next-POT) Hi-Z image dims
	// Padded pixels are initialised to 1.0 in mip 0
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_screen_width = 0;
	uint32_t m_screen_height = 0;
	uint32_t m_mip_levels = 0;
	uint32_t m_pass_count = 0;

	// Per-frame Hi-Z images (R32Sfloat with full mip chain)
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_hiz_images;

	// Per-mip image views for storage writes [frame][mip]
	std::array<std::vector<vk::raii::ImageView>, MAX_FRAMES_IN_FLIGHT> m_hiz_mip_views;

	vk::raii::Sampler m_nearest_sampler{nullptr};

	// 1x1 R32Sfloat dummy for unused output bindings
	std::unique_ptr<VeImage> m_dummy_image;

	std::unique_ptr<VeDescriptorSetLayout> m_downsample_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;

	// Per-pass descriptor sets [frame][pass_index]
	std::array<std::vector<vk::raii::DescriptorSet>, MAX_FRAMES_IN_FLIGHT> m_pass_sets;

	vk::Image m_depth_image{};
	vk::ImageView m_depth_image_view{};
};

} // namespace ve
