/* Registry - central ECS storage for entities and their components.
 *
 * Owns typed ComponentPools for each component type, a flat hierarchy array,
 * and a world-transform cache. Systems iterate pools directly for cache-friendly
 * access rather than chasing pointers through per-entity maps.
 */
#pragma once
#include "ve_export.hpp"
#include "ve_entity.hpp"
#include "ve_event.hpp"
#include "ve_component_pool.hpp"
#include "ve_component.hpp"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace ve {

enum class LightSource : uint8_t {
	Manual = 0,   // manually created in scene code
	Punctual,     // KHR_lights_punctual extension
	Emissive      // extracted from emissive materials
};

struct EntityMeta {
	std::string name;
	bool active = true;
	bool alive = false;
	uint16_t generation = 0;
	LightSource light_source = LightSource::Manual;
};

struct HierarchyEntry {
	Entity parent = Entity::null();
	Entity first_child = Entity::null();
	Entity next_sibling = Entity::null();
};

struct WorldTransformCache {
	glm::mat4 world_transform{1.0f};
	glm::mat3 world_normal{1.0f};
	bool transform_dirty = true;
	bool normal_dirty    = true;
};

class VENGINE_API Registry {
public:
	Registry();
	~Registry();

	// Entity lifecycle
	Entity createEntity(const std::string& name = "");
	void destroyEntity(Entity e);
	void destroyEntityRecursive(Entity e);
	bool isAlive(Entity e) const;
	uint32_t entityCount() const { return m_alive_count; }
	uint32_t maxEntityIndex() const { return static_cast<uint32_t>(m_meta.size()); }
	bool isAliveAtIndex(uint32_t index) const;

	// Entity metadata
	const std::string& getName(Entity e) const;
	void setName(Entity e, std::string name);
	bool isActive(Entity e) const;
	void setActive(Entity e, bool active);
	LightSource getLightSource(Entity e) const;
	void setLightSource(Entity e, LightSource source);

	// Component access (typed, dispatched via if-constexpr)
	template <typename T, typename... Args>
	T& addComponent(Entity e, Args&&... args);

	template <typename T>
	void removeComponent(Entity e);

	template <typename T>
	T* getComponent(Entity e);

	template <typename T>
	const T* getComponent(Entity e) const;

	template <typename T>
	bool hasComponent(Entity e) const;

	// Generic pool accessor
	template <typename T>
	ComponentPool<T>& pool();
	template <typename T>
	const ComponentPool<T>& pool() const;

	// Direct pool access (for system iteration)
	ComponentPool<TransformComponent>& transforms() { return m_transforms; }
	const ComponentPool<TransformComponent>& transforms() const { return m_transforms; }

	ComponentPool<MeshComponent>& meshes() { return m_meshes; }
	const ComponentPool<MeshComponent>& meshes() const { return m_meshes; }

	ComponentPool<PointLightComponent>& pointLights() { return m_point_lights; }
	const ComponentPool<PointLightComponent>& pointLights() const { return m_point_lights; }
	uint32_t activePointLightCount() const;

	ComponentPool<DirectionalLightComponent>& directionalLights() { return m_directional_lights; }
	const ComponentPool<DirectionalLightComponent>& directionalLights() const { return m_directional_lights; }
	uint32_t activeDirectionalLightCount() const;

	// Fast active check (skips generation validation)
	bool isActiveAtIndex(uint32_t index) const {
		return index < m_meta.size() && m_meta[index].active;
	}

	// Multi-component view factory
	template <typename... Components>
	auto view();

	// Hierarchy
	void   setParent(Entity child, Entity parent);
	Entity getParent(Entity e) const;
	Entity firstChild(Entity e) const;
	Entity nextSibling(Entity e) const;
	bool   hasParent(Entity e) const;

	// World transforms
	const glm::mat4& getWorldTransform(Entity e) const;
	const glm::mat3& getWorldNormal(Entity e) const;
	void invalidateWorldTransform(Entity e);

	// Convenience factories
	Entity createGameObject(const std::string& name = "");
	Entity createPointLight(float intensity = 1.0f, float radius = 1.0f,
	                        glm::vec3 color = glm::vec3(1.0f));
	Entity createDirectionalLight(float intensity = 1.0f,
	                              glm::vec3 color = glm::vec3(1.0f),
	                              glm::vec3 direction = glm::vec3(0.f, -1.f, -1.f));

	void clear();

	// Deferred deletion (queued by DeleteEntityRequest events, processed at safe frame boundary)
	bool hasPendingDeletions() const { return !m_pending_deletions.empty(); }
	void processPendingDeletions();

	// Entity for a given raw entity index (reconstructs with current generation)
	Entity entityFromIndex(uint32_t index) const;

	// Event dispatcher (scoped to this registry's lifetime)
	EventDispatcher& events() { return m_events; }
	const EventDispatcher& events() const { return m_events; }

private:
	void ensureSlotSize(uint32_t index);
	void invalidateMeshWorldAABBs(Entity e);

	// Entity management
	std::vector<EntityMeta> m_meta;
	std::vector<uint32_t> m_free_indices;
	uint32_t m_alive_count = 0;

	// Component pools
	ComponentPool<TransformComponent>        m_transforms;
	ComponentPool<MeshComponent>             m_meshes;
	ComponentPool<PointLightComponent>       m_point_lights;
	ComponentPool<DirectionalLightComponent> m_directional_lights;

	// Active light counters (maintained by setActive / addComponent / removeComponent)
	uint32_t m_active_point_lights = 0;
	uint32_t m_active_directional_lights = 0;

	// Hierarchy (indexed by entity index)
	std::vector<HierarchyEntry> m_hierarchy;

	// World transform cache (indexed by entity index)
	mutable std::vector<WorldTransformCache> m_world_cache;

	EventDispatcher m_events;
	std::vector<DeleteEntityRequest> m_pending_deletions;
};

// ── Template implementations ────────────────────────────────────────────────

template <typename T>
ComponentPool<T>& Registry::pool() {
	if constexpr (std::is_same_v<T, TransformComponent>)            return m_transforms;
	else if constexpr (std::is_same_v<T, MeshComponent>)            return m_meshes;
	else if constexpr (std::is_same_v<T, PointLightComponent>)      return m_point_lights;
	else if constexpr (std::is_same_v<T, DirectionalLightComponent>) return m_directional_lights;
	else static_assert(sizeof(T) == 0, "Unknown component type");
}

template <typename T>
const ComponentPool<T>& Registry::pool() const {
	if constexpr (std::is_same_v<T, TransformComponent>)            return m_transforms;
	else if constexpr (std::is_same_v<T, MeshComponent>)            return m_meshes;
	else if constexpr (std::is_same_v<T, PointLightComponent>)      return m_point_lights;
	else if constexpr (std::is_same_v<T, DirectionalLightComponent>) return m_directional_lights;
	else static_assert(sizeof(T) == 0, "Unknown component type");
}

template <typename T, typename... Args>
T& Registry::addComponent(Entity e, Args&&... args) {
	assert(isAlive(e) && "Entity is not alive");
	uint32_t idx = e.index();
	T* comp = nullptr;
	if constexpr (std::is_same_v<T, TransformComponent>) {
		comp = &m_transforms.emplace(idx, std::forward<Args>(args)...);
	} else if constexpr (std::is_same_v<T, MeshComponent>) {
		comp = &m_meshes.emplace(idx, std::forward<Args>(args)...);
	} else if constexpr (std::is_same_v<T, PointLightComponent>) {
		comp = &m_point_lights.emplace(idx, std::forward<Args>(args)...);
		if (m_meta[idx].active) m_active_point_lights++;
	} else if constexpr (std::is_same_v<T, DirectionalLightComponent>) {
		comp = &m_directional_lights.emplace(idx, std::forward<Args>(args)...);
		if (m_meta[idx].active) m_active_directional_lights++;
	} else {
		static_assert(sizeof(T) == 0, "Unknown component type");
	}
	comp->setContext(e, this);
	m_events.emit(ComponentAddedEvent<T>{e, *comp});
	return *comp;
}

template <typename T>
void Registry::removeComponent(Entity e) {
	assert(isAlive(e) && "Entity is not alive");
	m_events.emit(ComponentRemovedEvent<T>{e});
	uint32_t idx = e.index();
	if constexpr (std::is_same_v<T, TransformComponent>) {
		m_transforms.remove(idx);
	} else if constexpr (std::is_same_v<T, MeshComponent>) {
		m_meshes.remove(idx);
	} else if constexpr (std::is_same_v<T, PointLightComponent>) {
		if (m_meta[idx].active) 
			m_active_point_lights--;
		m_point_lights.remove(idx);
	} else if constexpr (std::is_same_v<T, DirectionalLightComponent>) {
		if (m_meta[idx].active) 
			m_active_directional_lights--;
		m_directional_lights.remove(idx);
	} else {
		static_assert(sizeof(T) == 0, "Unknown component type");
	}
}

template <typename T>
T* Registry::getComponent(Entity e) {
	if (!isAlive(e)) return nullptr;
	uint32_t idx = e.index();
	if constexpr (std::is_same_v<T, TransformComponent>) {
		return m_transforms.get(idx);
	} else if constexpr (std::is_same_v<T, MeshComponent>) {
		return m_meshes.get(idx);
	} else if constexpr (std::is_same_v<T, PointLightComponent>) {
		return m_point_lights.get(idx);
	} else if constexpr (std::is_same_v<T, DirectionalLightComponent>) {
		return m_directional_lights.get(idx);
	} else {
		static_assert(sizeof(T) == 0, "Unknown component type");
		return nullptr;
	}
}

template <typename T>
const T* Registry::getComponent(Entity e) const {
	if (!isAlive(e)) return nullptr;
	uint32_t idx = e.index();
	if constexpr (std::is_same_v<T, TransformComponent>) {
		return m_transforms.get(idx);
	} else if constexpr (std::is_same_v<T, MeshComponent>) {
		return m_meshes.get(idx);
	} else if constexpr (std::is_same_v<T, PointLightComponent>) {
		return m_point_lights.get(idx);
	} else if constexpr (std::is_same_v<T, DirectionalLightComponent>) {
		return m_directional_lights.get(idx);
	} else {
		static_assert(sizeof(T) == 0, "Unknown component type");
		return nullptr;
	}
}

template <typename T>
bool Registry::hasComponent(Entity e) const {
	if (!isAlive(e)) return false;
	uint32_t idx = e.index();
	if constexpr (std::is_same_v<T, TransformComponent>) {
		return m_transforms.has(idx);
	} else if constexpr (std::is_same_v<T, MeshComponent>) {
		return m_meshes.has(idx);
	} else if constexpr (std::is_same_v<T, PointLightComponent>) {
		return m_point_lights.has(idx);
	} else if constexpr (std::is_same_v<T, DirectionalLightComponent>) {
		return m_directional_lights.has(idx);
	} else {
		static_assert(sizeof(T) == 0, "Unknown component type");
		return false;
	}
}

} // namespace ve

// View included after Registry is fully defined (View references Registry)
#include "ve_view.hpp"

namespace ve {
template <typename... Components>
auto Registry::view() {
	return View<Components...>(*this, pool<Components>()...);
}
} // namespace ve
