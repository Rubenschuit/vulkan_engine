#include "pch.hpp"
#include "resources/ve_material.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_buffer.hpp"

#include <glm/glm.hpp>

namespace ve {

VeMaterial::VeMaterial(VeResourceManager& resource_manager, const std::string& resource_id,
                       const std::filesystem::path& albedo_path,
                       const std::filesystem::path& normal_path,
                       const std::filesystem::path& metallic_roughness_path,
                       const std::filesystem::path& occlusion_path,
                       const std::filesystem::path& emissive_path,
                       MaterialAlphaProps alpha_props,
                       MaterialFactors factors,
                       VeDescriptorPool* pool, VeDescriptorSetLayout* layout,
                       bool flip_tex_coord_v)
	: Resource(resource_id),
	  m_resource_manager(&resource_manager),
	  m_albedo_path(albedo_path),
	  m_normal_path(normal_path),
	  m_metallic_roughness_path(metallic_roughness_path),
	  m_occlusion_path(occlusion_path),
	  m_emissive_path(emissive_path),
	  m_alpha_props(alpha_props),
	  m_factors(factors),
	  m_pool(pool),
	  m_layout(layout),
	  m_flip_tex_coord_v(flip_tex_coord_v) {}

VeMaterial::~VeMaterial() {
	unload();
}

bool VeMaterial::doLoad() {
	m_albedo_texture = VeTexture::loadOrDefault(*m_resource_manager, m_albedo_path, TextureType::ALBEDO);
	m_normal_texture = VeTexture::loadOrDefault(*m_resource_manager, m_normal_path, TextureType::NORMAL);
	m_metallic_roughness_texture = VeTexture::loadOrDefault(*m_resource_manager, m_metallic_roughness_path, TextureType::METALLIC_ROUGHNESS);
	m_occlusion_texture = VeTexture::loadOrDefault(*m_resource_manager, m_occlusion_path, TextureType::OCCLUSION);
	m_emissive_texture = VeTexture::loadOrDefault(*m_resource_manager, m_emissive_path, TextureType::EMISSIVE);

	// If any texture failed to load (e.g. KTX2 decode error), use defaults so the material still works
	if (!m_albedo_texture.isValid())
		m_albedo_texture = m_resource_manager->load<VeTexture>("default_albedo");
	if (!m_normal_texture.isValid())
		m_normal_texture = m_resource_manager->load<VeTexture>("default_normal");
	if (!m_metallic_roughness_texture.isValid())
		m_metallic_roughness_texture = m_resource_manager->load<VeTexture>("default_metallic_roughness");
	if (!m_occlusion_texture.isValid())
		m_occlusion_texture = m_resource_manager->load<VeTexture>("default_occlusion");
	if (!m_emissive_texture.isValid())
		m_emissive_texture = m_resource_manager->load<VeTexture>("default_emissive");
	if (!m_albedo_texture.isValid() || !m_normal_texture.isValid() || !m_metallic_roughness_texture.isValid() ||
	    !m_occlusion_texture.isValid() || !m_emissive_texture.isValid()) {
		return false;
	}

	if (m_pool && m_layout) {
		VeDevice& device = m_resource_manager->getDevice();
		m_material_ubo = std::make_unique<VeBuffer>(device, MATERIAL_UBO_SIZE, 1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		float ubo_data[16];
		writeMaterialUBO(ubo_data, m_factors);
		m_material_ubo->map();
		m_material_ubo->writeToBuffer(ubo_data, MATERIAL_UBO_SIZE);
		m_material_ubo->unmap();

		auto albedo_info = m_albedo_texture.get()->getDescriptorInfo();
		auto normal_info = m_normal_texture.get()->getDescriptorInfo();
		auto mr_info = m_metallic_roughness_texture.get()->getDescriptorInfo();
		auto occlusion_info = m_occlusion_texture.get()->getDescriptorInfo();
		auto emissive_info = m_emissive_texture.get()->getDescriptorInfo();
		auto ubo_info = m_material_ubo->getDescriptorInfo();
		vk::raii::DescriptorSet temp{nullptr};
		VeDescriptorWriter(*m_layout, *m_pool)
			.writeImage(0, &albedo_info)
			.writeImage(1, &normal_info)
			.writeImage(2, &mr_info)
			.writeImage(3, &occlusion_info)
			.writeImage(4, &emissive_info)
			.writeBuffer(5, &ubo_info)
			.build(temp);
		m_descriptor_set = std::move(temp);
	}

	return true;
}

void VeMaterial::doUnload() {
	m_descriptor_set.reset();
	m_material_ubo.reset();
	m_albedo_texture = ResourceHandle<VeTexture>{};
	m_normal_texture = ResourceHandle<VeTexture>{};
	m_metallic_roughness_texture = ResourceHandle<VeTexture>{};
	m_occlusion_texture = ResourceHandle<VeTexture>{};
	m_emissive_texture = ResourceHandle<VeTexture>{};
}

void VeMaterial::setMaterialFactors(const MaterialFactors& factors) {
	m_factors = factors;
	if (m_material_ubo) {
		float ubo_data[16];
		writeMaterialUBO(ubo_data, m_factors);
		m_material_ubo->map();
		m_material_ubo->writeToBuffer(ubo_data, MATERIAL_UBO_SIZE);
		m_material_ubo->unmap();
	}
}

vk::raii::DescriptorSet& VeMaterial::getDescriptorSet() {
	assert(m_descriptor_set && "VeMaterial has no descriptor set (created without pool/layout)");
	return *m_descriptor_set;
}

} // namespace ve
