#pragma once
// Single-image min/max depth pyramid for the screen-space Hi-Z traversals
// (SSR and RC), built from the current frame's resolved depth in one
// FidelityFX SPD dispatch. Owned by ScreenRayResources.
// R32G32Sfloat: .x = per-tile MIN (farthest), .y = MAX (closest).

#include "ve_export.hpp"
#include "ve_config.hpp"

#include <memory>
#include <vector>
#include <filesystem>

namespace ve {
class VeDevice;
class VeImage;
class VeBuffer;
class VeDescriptorPool;
class VeDescriptorSetLayout;
class VeComputePipeline;
}

namespace ve {

class VENGINE_API MinMaxDepthPyramid {
public:
	MinMaxDepthPyramid(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		vk::Extent2D depth_extent,
		const vk::raii::ImageView& depth_image_view,
		const std::filesystem::path& shaders_dir);
	~MinMaxDepthPyramid();

	MinMaxDepthPyramid(const MinMaxDepthPyramid&) = delete;
	MinMaxDepthPyramid& operator=(const MinMaxDepthPyramid&) = delete;

	// Pre: depth in eDepthStencilReadOnlyOptimal.
	// Post: all pyramid mips in eShaderReadOnlyOptimal.
	// May be recorded on the graphics or the dedicated compute queue.
	void generate(vk::raii::CommandBuffer& cmd);

	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D depth_extent,
	              const vk::raii::ImageView& depth_image_view);

	const vk::raii::ImageView& getPyramidView() const;
	// Raw image handle for mip-0 copies (RC's prev-frame reprojection check).
	vk::Image getImage() const;
	uint32_t getMipLevels() const { return m_mip_levels; }
	// Mip 0 extent (= padded source / 2); consumers map full-res pixel p to
	// mip-L texel p >> (L + 1).
	vk::Extent2D getMip0Extent() const { return {m_width, m_height}; }

private:
	void createImage(vk::Extent2D depth_extent);
	void createMipViews();
	void createSampler();
	void createComputeSetLayout();
	void createPipelineLayout();
	void createPipeline(const std::filesystem::path& shaders_dir);
	void createAtomicCounterBuffer();
	void createDescriptorSet(VeDescriptorPool& pool);

	VeDevice& m_ve_device;

	// SPD's hard cap is 12 mips per dispatch.
	static constexpr uint32_t SPD_MAX_MIPS = 12;

	// m_width/m_height
	// m_padded_source_*      = source extent rounded up to next POT.
	// m_screen_width/height
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_padded_source_width = 0;
	uint32_t m_padded_source_height = 0;
	uint32_t m_screen_width = 0;
	uint32_t m_screen_height = 0;
	uint32_t m_mip_levels = 0;

	std::unique_ptr<VeImage> m_image;
	std::vector<vk::raii::ImageView> m_mip_views;

	vk::raii::Sampler m_point_sampler{nullptr};

	// 4-byte storage buffer holding SPD's global atomic counter. SPD resets it
	// to 0 at the end of every dispatch, so a single zero-init at creation
	// suffices; successive dispatches are barrier-ordered on the graphics queue.
	std::unique_ptr<VeBuffer> m_atomic_counter_buffer;

	std::unique_ptr<VeDescriptorSetLayout> m_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;
	vk::raii::DescriptorSet m_descriptor_set{nullptr};

	vk::ImageView m_depth_image_view{};
};

}