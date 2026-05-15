/* Resource base class for the resource management system.
 * Based on:
 * https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Engine_Architecture/04_resource_management.html
 */
#pragma once
#include "ve_export.hpp"

#include <string>

namespace ve {

class EventBus;

class VENGINE_API Resource {
public:
	explicit Resource(const std::string& resource_id);
	virtual ~Resource() = default;

	Resource(const Resource&) = delete;
	Resource& operator=(const Resource&) = delete;

	const std::string& getId() const { return m_resource_id; }
	bool isLoaded() const { return m_loaded; }

	bool load();
	void unload();

	// Called by VeResourceManager just before doUnload. Subclasses override to
	// emit a typed ResourceUnloadingEvent<Derived> on the bus.
	virtual void emitUnloadingEvent(EventBus& /*bus*/) {}

protected:
	virtual bool doLoad() = 0;
	virtual void doUnload() = 0;

	std::string m_resource_id;
	bool m_loaded = false;

	// For subclasses that create resources in constructor (e.g. createDefault texture)
	void setLoaded(bool loaded) { m_loaded = loaded; }
};

} // namespace ve
