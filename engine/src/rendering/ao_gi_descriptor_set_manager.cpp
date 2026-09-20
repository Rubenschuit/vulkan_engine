#include "pch.hpp"
#include "rendering/ao_gi_descriptor_set_manager.hpp"
#include "rendering/gtao_system.hpp"
#include "rendering/rc_system.hpp"

namespace ve {

AoGiDescriptorSetManager::AoGiDescriptorSetManager(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	VeResourceManager& resource_manager,
	const GtaoSystem& gtao_system,
	const RcSystem& rc_system)
	: m_ve_device(device), m_gtao_system(gtao_system), m_rc_system(rc_system) {
	m_default_ao_texture = resource_manager.load<VeTexture>("default_albedo");
	m_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment) // AO
		.addBinding(1, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment)      // AO sampler
		.addBinding(2, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment) // RC irradiance
		.addBinding(3, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment)      // RC sampler
		.addBinding(4, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment) // RC sky visibility
		.addBinding(5, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment) // RC rough spec
		.addBinding(6, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment) // RC geometry
		.build();
	rewrite(descriptor_pool);
}

AoGiDescriptorSetManager::~AoGiDescriptorSetManager() = default;

void AoGiDescriptorSetManager::rewrite(VeDescriptorPool& descriptor_pool) {
	vk::DescriptorImageInfo ao_sampler_info{
		.sampler = *m_gtao_system.outputSampler(),
	};
	vk::DescriptorImageInfo ao_default_info{
		.imageView = *m_default_ao_texture->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo rc_dummy_info{
		.imageView = *m_rc_system.getDummyImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo rc_sampler_info{
		.sampler = *m_rc_system.getOutputSampler(),
	};
	vk::DescriptorImageInfo visibility_dummy_info{
		.imageView = *m_rc_system.getVisibilityDummyImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};

	const bool rc_allocated = m_rc_system.isAllocated();
	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		vk::DescriptorImageInfo ao_live_info{
			.imageView = *m_gtao_system.outputView(frame),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo rc_live_info{
			.imageView = rc_allocated ? *m_rc_system.getOutputImageView(frame) : *m_rc_system.getDummyImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo visibility_live_info{
			.imageView = rc_allocated ? *m_rc_system.getVisibilityImageView(frame) : *m_rc_system.getVisibilityDummyImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo spec_live_info{
			.imageView = rc_allocated ? *m_rc_system.getSpecImageView(frame) : *m_rc_system.getDummyImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo geometry_live_info{
			.imageView = rc_allocated ? *m_rc_system.getGeometryImageView(frame) : *m_rc_system.getDummyImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};

		VeDescriptorWriter(*m_layout, descriptor_pool)
			.writeImage(0, &ao_live_info)
			.writeImage(1, &ao_sampler_info)
			.writeImage(2, &rc_live_info)
			.writeImage(3, &rc_sampler_info)
			.writeImage(4, &visibility_live_info)
			.writeImage(5, &spec_live_info)
			.writeImage(6, &geometry_live_info)
			.build(m_live_sets[frame]);

		VeDescriptorWriter(*m_layout, descriptor_pool)
			.writeImage(0, &ao_live_info)
			.writeImage(1, &ao_sampler_info)
			.writeImage(2, &rc_dummy_info)
			.writeImage(3, &rc_sampler_info)
			.writeImage(4, &visibility_dummy_info)
			.writeImage(5, &rc_dummy_info)
			.writeImage(6, &rc_dummy_info)
			.build(m_ao_only_sets[frame]);

		VeDescriptorWriter(*m_layout, descriptor_pool)
			.writeImage(0, &ao_default_info)
			.writeImage(1, &ao_sampler_info)
			.writeImage(2, &rc_live_info)
			.writeImage(3, &rc_sampler_info)
			.writeImage(4, &visibility_live_info)
			.writeImage(5, &spec_live_info)
			.writeImage(6, &geometry_live_info)
			.build(m_rc_only_sets[frame]);
	}

	VeDescriptorWriter(*m_layout, descriptor_pool)
		.writeImage(0, &ao_default_info)
		.writeImage(1, &ao_sampler_info)
		.writeImage(2, &rc_dummy_info)
		.writeImage(3, &rc_sampler_info)
		.writeImage(4, &visibility_dummy_info)
		.writeImage(5, &rc_dummy_info)
		.writeImage(6, &rc_dummy_info)
		.build(m_dummy_set);
}

vk::raii::DescriptorSet& AoGiDescriptorSetManager::select(uint32_t frame_index, bool ao_live, bool rc_live) {
	if (ao_live && rc_live)
		return m_live_sets[frame_index];
	if (ao_live)
		return m_ao_only_sets[frame_index];
	if (rc_live)
		return m_rc_only_sets[frame_index];
	return m_dummy_set;
}

}
