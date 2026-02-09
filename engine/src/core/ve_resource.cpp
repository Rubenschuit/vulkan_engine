#include "core/ve_resource.hpp"

namespace ve {

Resource::Resource(const std::string& resource_id) : m_resource_id(resource_id) {}

bool Resource::load() {
	if (m_loaded)
		return true;
	m_loaded = doLoad();
	return m_loaded;
}

void Resource::unload() {
	if (!m_loaded)
		return;
	doUnload();
	m_loaded = false;
}

} // namespace ve
