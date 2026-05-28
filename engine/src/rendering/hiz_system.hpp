#pragma once
// Builds a hierarchical-Z depth pyramid from the previous frame's depth
// buffer in one compute dispatch via AMD FidelityFX SPD. Consumed by the GPU
// occlusion-culling backends.
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "events/event_bus.hpp"

#include <memory>
#include <array>
#include <vector>

namespace ve {
class VeDevice;
class VeImage;
class VeBuffer;
class VeDescriptorPool;
class VeDescriptorSetLayout;
class VeComputePipeline;
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

	// Pre: depth in eDepthStencilReadOnlyOptimal.
	// Post: all Hi-Z mips in eShaderReadOnlyOptimal.
	void generate(vk::raii::CommandBuffer& cmd, uint32_t frame_index);

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
	void createComputeSetLayout();
	void createPipelineLayout();
	void createPipeline(const std::filesystem::path& shaders_dir);
	void createAtomicCounterBuffers();
	void createDescriptorSets(VeDescriptorPool& pool);

	VeDevice& m_ve_device;

	// SPD's hard cap is 12 mips per dispatch.
	static constexpr uint32_t SPD_MAX_MIPS = 12;

	// m_width/m_height       = Hi-Z image extent = padded_source / 2 (SPD destination mip 0).
	// m_padded_source_*      = source extent rounded up to next POT.
	// m_screen_width/height  = exact depth-buffer (source) extent.
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_padded_source_width = 0;
	uint32_t m_padded_source_height = 0;
	uint32_t m_screen_width = 0;
	uint32_t m_screen_height = 0;
	uint32_t m_mip_levels = 0;

	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_hiz_images;

	std::array<std::vector<vk::raii::ImageView>, MAX_FRAMES_IN_FLIGHT> m_hiz_mip_views;

	vk::raii::Sampler m_nearest_sampler{nullptr};

	// One 4-byte storage buffer per frame holding SPD's global atomic counter.
	// SPD resets the counter back to 0 at the end of every dispatch, so a single
	// zero-init at creation suffices.
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_atomic_counter_buffers;

	std::unique_ptr<VeDescriptorSetLayout> m_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_compute_pipeline;

	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();

	vk::Image m_depth_image{};
	vk::ImageView m_depth_image_view{};

	EventBus* m_event_bus = nullptr;
	EventSubscriptionId m_resolution_sub = 0;
};

} // namespace ve
