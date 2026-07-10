#pragma once
// Single-image min/max depth pyramid for the SSR Hi-Z traversal, built from
// the current frame's resolved depth in one FidelityFX SPD dispatch.
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

class VENGINE_API SsrHizPyramid {
public:
	SsrHizPyramid(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		vk::Extent2D depth_extent,
		const vk::raii::ImageView& depth_image_view,
		const std::filesystem::path& shaders_dir);
	~SsrHizPyramid();

	SsrHizPyramid(const SsrHizPyramid&) = delete;
	SsrHizPyramid& operator=(const SsrHizPyramid&) = delete;

	// Pre: depth in eDepthStencilReadOnlyOptimal.
	// Post: all pyramid mips in eShaderReadOnlyOptimal.
	// Must be recorded on the graphics timeline.
	void generate(vk::raii::CommandBuffer& cmd);

	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D depth_extent,
	              const vk::raii::ImageView& depth_image_view);

	const vk::raii::ImageView& getPyramidView() const;
	uint32_t getMipLevels() const { return m_mip_levels; }

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

	// m_width/m_height       = pyramid image extent = padded_source / 2 (SPD destination mip 0).
	// m_padded_source_*      = source extent rounded up to next POT.
	// m_screen_width/height  = exact depth-buffer (source) extent.
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