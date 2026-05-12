#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_texture.hpp"

#include <unordered_map>
#include <vector>

namespace ve {

class VENGINE_API BindlessTextureRegistry {
public:
	BindlessTextureRegistry(VeDevice& device, uint32_t max_textures = MAX_BINDLESS_TEXTURES);
	~BindlessTextureRegistry();

	BindlessTextureRegistry(const BindlessTextureRegistry&) = delete;
	BindlessTextureRegistry& operator=(const BindlessTextureRegistry&) = delete;

	uint32_t registerTexture(VeTexture* texture);
	void unregisterTexture(VeTexture* texture);

	const vk::raii::DescriptorSet& getDescriptorSet() const { return m_descriptor_set; }
	const vk::raii::DescriptorSetLayout& getSetLayout() const {
		return m_set_layout->getDescriptorSetLayout();
	}

	// For missing textures
	uint32_t getDefaultIndex(TextureType type) const;

	// Clear all non-default texture registrations (call when scene changes)
	void reset();

private:
	void writeSlot(uint32_t index, VeTexture* texture);

	VeDevice& m_ve_device;
	[[maybe_unused]] uint32_t m_max_textures;

	std::unique_ptr<VeDescriptorPool> m_pool;
	std::unique_ptr<VeDescriptorSetLayout> m_set_layout;
	vk::raii::DescriptorSet m_descriptor_set{nullptr};
	vk::raii::Sampler m_shared_sampler{nullptr};

	std::unordered_map<VeTexture*, uint32_t> m_texture_to_index;
	std::vector<uint32_t> m_free_list;
	uint32_t m_next_index = 0;

	// Default texture indices per type
	uint32_t m_default_albedo_index = 0;
	uint32_t m_default_normal_index = 0;
	uint32_t m_default_mr_index = 0;
	uint32_t m_default_occlusion_index = 0;
	uint32_t m_default_emissive_index = 0;
	uint32_t m_default_specular_index = 0;
	uint32_t m_default_specular_color_index = 0;

	// Default textures (kept alive)
	std::shared_ptr<VeTexture> m_default_albedo;
	std::shared_ptr<VeTexture> m_default_normal;
	std::shared_ptr<VeTexture> m_default_mr;
	std::shared_ptr<VeTexture> m_default_occlusion;
	std::shared_ptr<VeTexture> m_default_emissive;
	std::shared_ptr<VeTexture> m_default_specular;
	std::shared_ptr<VeTexture> m_default_specular_color;
};

} // namespace ve