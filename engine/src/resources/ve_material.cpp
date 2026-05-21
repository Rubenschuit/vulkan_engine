#include "pch.hpp"
#include "resources/ve_material.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

namespace ve {

VeMaterial::VeMaterial(VeResourceManager& resource_manager, const std::string& resource_id,
                       const std::filesystem::path& albedo_path,
                       const std::filesystem::path& normal_path,
                       const std::filesystem::path& metallic_roughness_path,
                       const std::filesystem::path& occlusion_path,
                       const std::filesystem::path& emissive_path,
                       const std::filesystem::path& specular_path,
                       const std::filesystem::path& specular_color_path,
                       MaterialAlphaProps alpha_props,
                       MaterialFactors factors,
                       bool flip_tex_coord_v)
	: Resource(resource_id),
	  m_resource_manager(&resource_manager),
	  m_albedo_path(albedo_path),
	  m_normal_path(normal_path),
	  m_metallic_roughness_path(metallic_roughness_path),
	  m_occlusion_path(occlusion_path),
	  m_emissive_path(emissive_path),
	  m_specular_path(specular_path),
	  m_specular_color_path(specular_color_path),
	  m_alpha_props(alpha_props),
	  m_factors(factors),
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
	m_specular_texture = VeTexture::loadOrDefault(*m_resource_manager, m_specular_path, TextureType::SPECULAR);
	m_specular_color_texture = VeTexture::loadOrDefault(*m_resource_manager, m_specular_color_path, TextureType::SPECULAR_COLOR);

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
	if (!m_specular_texture.isValid())
		m_specular_texture = m_resource_manager->load<VeTexture>("default_specular");
	if (!m_specular_color_texture.isValid())
		m_specular_color_texture = m_resource_manager->load<VeTexture>("default_specular_color");
	if (!m_albedo_texture.isValid() || !m_normal_texture.isValid() || !m_metallic_roughness_texture.isValid() ||
	    !m_occlusion_texture.isValid() || !m_emissive_texture.isValid() ||
	    !m_specular_texture.isValid() || !m_specular_color_texture.isValid()) {
		return false;
	}

	return true;
}

void VeMaterial::doUnload() {
	m_albedo_texture = ResourceHandle<VeTexture>{};
	m_normal_texture = ResourceHandle<VeTexture>{};
	m_metallic_roughness_texture = ResourceHandle<VeTexture>{};
	m_occlusion_texture = ResourceHandle<VeTexture>{};
	m_emissive_texture = ResourceHandle<VeTexture>{};
	m_specular_texture = ResourceHandle<VeTexture>{};
	m_specular_color_texture = ResourceHandle<VeTexture>{};
}

void VeMaterial::emitUnloadingEvent(EventBus& bus) {
	ResourceUnloadingEvent<VeMaterial> ev{};
	ev.resource = this;
	bus.emitImmediate(ev);
}

void VeMaterial::setMaterialFactors(const MaterialFactors& factors) {
	m_factors = factors;
}

} // namespace ve