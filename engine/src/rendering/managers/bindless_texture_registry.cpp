#include "pch.hpp"
#include "rendering/managers/bindless_texture_registry.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "utils/ve_log.hpp"

namespace ve {

BindlessTextureRegistry::BindlessTextureRegistry(VeDevice& device, EventBus& event_bus, uint32_t max_textures)
	: m_ve_device(device), m_event_bus(event_bus), m_max_textures(max_textures) {

	// Create pool with UPDATE_AFTER_BIND flag
	m_pool = VeDescriptorPool::Builder(m_ve_device)
		.setMaxSets(1)
		.addPoolSize(vk::DescriptorType::eSampledImage, max_textures)
		.addPoolSize(vk::DescriptorType::eSampler, 1)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind | vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
		.build();

	// Sampler at binding 0, texture array at binding 1 (variable count must be highest binding)
	m_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampler,
			vk::ShaderStageFlagBits::eFragment, 1)
		.addBinding(1, vk::DescriptorType::eSampledImage,
			vk::ShaderStageFlagBits::eFragment, max_textures)
		.setBindingFlags(0, vk::DescriptorBindingFlagBits::eUpdateAfterBind)
		.setBindingFlags(1,
			vk::DescriptorBindingFlagBits::ePartiallyBound |
			vk::DescriptorBindingFlagBits::eUpdateAfterBind |
			vk::DescriptorBindingFlagBits::eVariableDescriptorCount)
		.setLayoutFlags(vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool)
		.build();

	m_pool->allocateDescriptorVariableCount(
		m_set_layout->getDescriptorSetLayout(), m_descriptor_set, max_textures);

	// Create shared sampler
	auto props = m_ve_device.getPhysicalDevice().getProperties();
	float max_aniso = std::min(props.limits.maxSamplerAnisotropy, 16.0f);
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eLinear,
		.addressModeU = vk::SamplerAddressMode::eRepeat,
		.addressModeV = vk::SamplerAddressMode::eRepeat,
		.addressModeW = vk::SamplerAddressMode::eRepeat,
		.mipLodBias = 0.0f,
		.anisotropyEnable = VK_TRUE,
		.maxAnisotropy = max_aniso,
		.compareEnable = VK_FALSE,
		.minLod = 0.0f,
		.maxLod = VK_LOD_CLAMP_NONE,
		.borderColor = vk::BorderColor::eIntOpaqueBlack,
		.unnormalizedCoordinates = VK_FALSE,
	};
	m_shared_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);

	// Write the shared sampler to binding 0
	vk::DescriptorImageInfo sampler_write{
		.sampler = *m_shared_sampler,
	};
	vk::WriteDescriptorSet write{
		.dstSet = *m_descriptor_set,
		.dstBinding = 0,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = vk::DescriptorType::eSampler,
		.pImageInfo = &sampler_write,
	};
	m_ve_device.getDevice().updateDescriptorSets(write, {});

	// Create and register default textures
	m_default_albedo = VeTexture::createDefault(m_ve_device, TextureType::ALBEDO);
	m_default_normal = VeTexture::createDefault(m_ve_device, TextureType::NORMAL);
	m_default_mr = VeTexture::createDefault(m_ve_device, TextureType::METALLIC_ROUGHNESS);
	m_default_occlusion = VeTexture::createDefault(m_ve_device, TextureType::OCCLUSION);
	m_default_emissive = VeTexture::createDefault(m_ve_device, TextureType::EMISSIVE);
	m_default_specular = VeTexture::createDefault(m_ve_device, TextureType::SPECULAR);
	m_default_specular_color = VeTexture::createDefault(m_ve_device, TextureType::SPECULAR_COLOR);

	// Populate default indices in the registry so they can be used immediately
	m_default_albedo_index = indexFor(m_default_albedo.get());
	m_default_normal_index = indexFor(m_default_normal.get());
	m_default_mr_index = indexFor(m_default_mr.get());
	m_default_occlusion_index = indexFor(m_default_occlusion.get());
	m_default_emissive_index = indexFor(m_default_emissive.get());
	m_default_specular_index = indexFor(m_default_specular.get());
	m_default_specular_color_index = indexFor(m_default_specular_color.get());

	VE_LOGI("BindlessTextureRegistry: max=" << max_textures
		<< ", defaults registered (albedo=" << m_default_albedo_index
		<< ", normal=" << m_default_normal_index << ")");

	m_unload_sub = m_event_bus.subscribe<ResourceUnloadingEvent<VeTexture>>(
		[this](const ResourceUnloadingEvent<VeTexture>& e) {
			releaseSlot(e.resource);
		});
}

BindlessTextureRegistry::~BindlessTextureRegistry() {
	if (m_unload_sub != NO_SUB)
		m_event_bus.unsubscribe<ResourceUnloadingEvent<VeTexture>>(m_unload_sub);
}

uint32_t BindlessTextureRegistry::indexFor(VeTexture* texture) {
	auto it = m_texture_to_index.find(texture);
	if (it != m_texture_to_index.end())
		return it->second;

	uint32_t index;
	if (!m_free_list.empty()) {
		index = m_free_list.back();
		m_free_list.pop_back();
	} else {
		assert(m_next_index < m_max_textures && "Bindless texture registry full");
		index = m_next_index++;
	}

	m_texture_to_index[texture] = index;
	writeSlot(index, texture);
	return index;
}

void BindlessTextureRegistry::releaseSlot(VeTexture* texture) {
	auto it = m_texture_to_index.find(texture);
	if (it == m_texture_to_index.end())
		return;

	uint32_t index = it->second;
	m_texture_to_index.erase(it);

	// Overwrite slot with default albedo so the descriptor doesn't reference
	// a destroyed image view.
	writeSlot(index, m_default_albedo.get());
	m_free_list.push_back(index);
}

void BindlessTextureRegistry::reset() {
	// Overwrite all non-default slots with the default albedo texture
	// so no descriptor points to a destroyed image view.
	static constexpr uint32_t NUM_DEFAULTS = 7;
	for (auto& [tex, index] : m_texture_to_index) {
		if (index >= NUM_DEFAULTS)
			writeSlot(index, m_default_albedo.get());
	}

	// Re-register only the default textures
	m_texture_to_index.clear();
	m_free_list.clear();
	m_next_index = 0;
	m_texture_to_index[m_default_albedo.get()] = m_default_albedo_index;
	m_texture_to_index[m_default_normal.get()] = m_default_normal_index;
	m_texture_to_index[m_default_mr.get()] = m_default_mr_index;
	m_texture_to_index[m_default_occlusion.get()] = m_default_occlusion_index;
	m_texture_to_index[m_default_emissive.get()] = m_default_emissive_index;
	m_texture_to_index[m_default_specular.get()] = m_default_specular_index;
	m_texture_to_index[m_default_specular_color.get()] = m_default_specular_color_index;
	m_next_index = NUM_DEFAULTS;
}

uint32_t BindlessTextureRegistry::getDefaultIndex(TextureType type) const {
	switch (type) {
		case TextureType::ALBEDO: return m_default_albedo_index;
		case TextureType::NORMAL: return m_default_normal_index;
		case TextureType::METALLIC_ROUGHNESS: return m_default_mr_index;
		case TextureType::OCCLUSION: return m_default_occlusion_index;
		case TextureType::EMISSIVE: return m_default_emissive_index;
		case TextureType::SPECULAR: return m_default_specular_index;
		case TextureType::SPECULAR_COLOR: return m_default_specular_color_index;
		default: return m_default_albedo_index;
	}
}

void BindlessTextureRegistry::writeSlot(uint32_t index, VeTexture* texture) {
	vk::DescriptorImageInfo image_info{
		.imageView = *texture->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::WriteDescriptorSet write{
		.dstSet = *m_descriptor_set,
		.dstBinding = 1,
		.dstArrayElement = index,
		.descriptorCount = 1,
		.descriptorType = vk::DescriptorType::eSampledImage,
		.pImageInfo = &image_info,
	};
	m_ve_device.getDevice().updateDescriptorSets(write, {});
}

} // namespace ve