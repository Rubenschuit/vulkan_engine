#include "pch.hpp"
#include "resources/ve_material.hpp"
#include "vulkan/ve_descriptors.hpp"

namespace ve {

VeMaterial::VeMaterial(VeResourceManager& resource_manager, const std::string& resource_id,
                       const std::filesystem::path& albedo_path,
                       const std::filesystem::path& normal_path,
                       const std::filesystem::path& metallic_roughness_path,
                       MaterialAlphaProps alpha_props,
                       VeDescriptorPool* pool, VeDescriptorSetLayout* layout)
	: Resource(resource_id),
	  m_resource_manager(&resource_manager),
	  m_albedo_path(albedo_path),
	  m_normal_path(normal_path),
	  m_metallic_roughness_path(metallic_roughness_path),
	  m_alpha_props(alpha_props),
	  m_pool(pool),
	  m_layout(layout) {}

VeMaterial::~VeMaterial() {
	unload();
}

bool VeMaterial::doLoad() {
	m_albedo_texture = VeTexture::loadOrDefault(*m_resource_manager, m_albedo_path, TextureType::ALBEDO, vk::Format::eR8G8B8A8Srgb);
	m_normal_texture = VeTexture::loadOrDefault(*m_resource_manager, m_normal_path, TextureType::NORMAL, vk::Format::eR8G8B8A8Unorm);
	m_metallic_roughness_texture = VeTexture::loadOrDefault(*m_resource_manager, m_metallic_roughness_path, TextureType::METALLIC_ROUGHNESS, vk::Format::eR8G8B8A8Unorm);

	if (!m_albedo_texture.isValid() || !m_normal_texture.isValid() || !m_metallic_roughness_texture.isValid()) {
		return false;
	}

	if (m_pool && m_layout) {
		auto albedo_info = m_albedo_texture.get()->getDescriptorInfo();
		auto normal_info = m_normal_texture.get()->getDescriptorInfo();
		auto mr_info = m_metallic_roughness_texture.get()->getDescriptorInfo();
		vk::raii::DescriptorSet temp{nullptr};
		VeDescriptorWriter(*m_layout, *m_pool)
			.writeImage(0, &albedo_info)
			.writeImage(1, &normal_info)
			.writeImage(2, &mr_info)
			.build(temp);
		m_descriptor_set = std::move(temp);
	}

	return true;
}

void VeMaterial::doUnload() {
	m_descriptor_set.reset();
	m_albedo_texture = ResourceHandle<VeTexture>{};
	m_normal_texture = ResourceHandle<VeTexture>{};
	m_metallic_roughness_texture = ResourceHandle<VeTexture>{};
}

vk::raii::DescriptorSet& VeMaterial::getDescriptorSet() {
	assert(m_descriptor_set && "VeMaterial has no descriptor set (created without pool/layout)");
	return *m_descriptor_set;
}

} // namespace ve
