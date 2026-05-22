#include "pch.hpp"
#include "resources/ve_material.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

namespace ve {

VeMaterial::VeMaterial(const std::string& resource_id,
                       MaterialTextures textures,
                       MaterialAlphaProps alpha_props,
                       MaterialFactors factors,
                       bool flip_tex_coord_v)
	: Resource(resource_id),
	  m_textures(std::move(textures)),
	  m_alpha_props(alpha_props),
	  m_factors(factors),
	  m_flip_tex_coord_v(flip_tex_coord_v) {
	setLoaded(true);
}

VeMaterial::~VeMaterial() {
	unload();
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