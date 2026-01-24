// This system implements the bloom effect.
// It uses a mip chain to downsample the input image and then upsample it to create the bloom effect.
// The downsampling is done using a 13-sample kernel with karis weights on the first pass to suppress fireflies.
// The upsampling is done using a 9 sample kernel with a box filter.
// The final output is stored in the first mip of the mip chain.
// Based on the article https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom.

#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "core/ve_device.hpp"
#include "core/ve_pipeline.hpp"
#include "core/ve_descriptors.hpp"
#include "core/ve_image.hpp"
#include "game/ve_frame_info.hpp"

#include <memory>
#include <vector>
#include <filesystem>

namespace ve {

struct BloomMip {
	std::unique_ptr<VeImage> image;
	vk::raii::DescriptorSet descriptor_set{nullptr};
};

class VENGINE_API BloomSystem {
public:
	BloomSystem(
		VeDevice& device,
		vk::Extent2D extent,
		const vk::raii::ImageView& input_image_view,
		std::filesystem::path downsample_shader_path,
		std::filesystem::path upsample_shader_path);
	~BloomSystem();

	BloomSystem(const BloomSystem&) = delete;
	BloomSystem& operator=(const BloomSystem&) = delete;

	void render(vk::raii::CommandBuffer& command_buffer);
	void recreateResources(vk::Extent2D extent, const vk::raii::ImageView& input_image_view);

	const vk::raii::ImageView& getBloomTexture() const { return m_bloom_mips[0].image->getImageView(); }

private:
	void createDescriptorSetLayout();
	void createDescriptorPool();
	void createDescriptorSets(const vk::raii::ImageView& input_image_view);
	void createMipChain(vk::Extent2D extent);
	void createPipelineLayouts();
	void createPipelines();

	VeDevice& m_ve_device;
	std::filesystem::path m_downsample_shader_path;
	std::filesystem::path m_upsample_shader_path;

	std::unique_ptr<VeDescriptorSetLayout> m_descriptor_set_layout;
	std::unique_ptr<VeDescriptorPool> m_descriptor_pool;
	std::unique_ptr<vk::raii::Sampler> m_sampler;

	std::vector<BloomMip> m_bloom_mips;
	vk::raii::DescriptorSet m_hdr_input_descriptor_set{nullptr};

	vk::raii::PipelineLayout m_downsample_pipeline_layout{nullptr};
	vk::raii::PipelineLayout m_upsample_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_downsample_pipeline;
	std::unique_ptr<VePipeline> m_upsample_pipeline;

	const uint32_t m_mip_count = 7;
};

} // namespace ve

