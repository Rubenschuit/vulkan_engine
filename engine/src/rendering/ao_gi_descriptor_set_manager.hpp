/*
 * AoGiDescriptorSetManager: handles the composite's ambient occlusion and GI inputs.
 * One layout and per-frame variants for AO live/dummy x RC live/dummy,
 * written from GtaoSystem's output and RcSystem's outputs. RenderPipeline
 * rewrites it after either producer's images change.
 */

#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"

#include <array>
#include <memory>

namespace ve {

class GtaoSystem;
class RcSystem;

class VENGINE_API AoGiDescriptorSetManager {
public:
	AoGiDescriptorSetManager(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		VeResourceManager& resource_manager,
		const GtaoSystem& gtao_system,
		const RcSystem& rc_system);
	~AoGiDescriptorSetManager();

	AoGiDescriptorSetManager(const AoGiDescriptorSetManager&) = delete;
	AoGiDescriptorSetManager& operator=(const AoGiDescriptorSetManager&) = delete;

	const vk::raii::DescriptorSetLayout& layout() const { return m_layout->getDescriptorSetLayout(); }

	// Rebuilds every variant from the producers' current images. Device idle.
	void rewrite(VeDescriptorPool& descriptor_pool);

	// Variant for the frame: AO live/dummy x RC live/dummy. Dummies are
	// identity in the composite (AO = 1, RC confidence = 0).
	vk::raii::DescriptorSet& select(uint32_t frame_index, bool ao_live, bool rc_live);

private:
	VeDevice& m_ve_device;
	const GtaoSystem& m_gtao_system;
	const RcSystem& m_rc_system;
	ResourceHandle<VeTexture> m_default_ao_texture; // white
	std::unique_ptr<VeDescriptorSetLayout> m_layout;
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_live_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_ao_only_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_rc_only_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	vk::raii::DescriptorSet m_dummy_set{nullptr};
};

}
