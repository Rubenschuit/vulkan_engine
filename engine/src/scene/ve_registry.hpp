/* Registry - central ECS storage for entities and their components.
 *
 * Owns typed ComponentPools for each component type, a flat hierarchy array,
 * and a world-transform cache. Systems iterate pools directly for cache-friendly
 * access rather than chasing pointers through per-entity maps.
 */
#pragma once
#include "ve_export.hpp"
#include "ve_entity.hpp"
#include "ecs_event_dispatcher.hpp"
#include "ve_component_pool.hpp"
#include "ve_component.hpp"

#include <glm/glm.hpp>
#include <string>
#include <tuple>
#include <vector>
#include <span>

namespace ve {

enum class LightSource : uint8_t {
	Manual = 0,   // manually created in scene code
	Punctual,     // KHR_lights_punctual extension
	Emissive      // extracted from emissive materials
};

struct EntityMeta {
	std::string name;
	bool active = true;
	bool active_in_hierarchy = true;
	bool alive = false;
	bool animated = false;
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

class Registry;

// Deferred component removal (processed alongside entity deletions at safe frame boundary)
struct PendingComponentRemoval {
	Entity entity;
	void (*remove_fn)(Registry&, Entity);
};

using ComponentPools = std::tuple<
	ComponentPool<TransformComponent>,
	ComponentPool<MeshComponent>,
	ComponentPool<PointLightComponent>,
	ComponentPool<DirectionalLightComponent>,
	ComponentPool<SpotLightComponent>,
	ComponentPool<AreaLightComponent>,
	ComponentPool<RigidbodyComponent>,
	ComponentPool<AnimatorComponent>,
	ComponentPool<SkinComponent>,
	ComponentPool<CameraComponent>,
	ComponentPool<ParticleEmitterComponent>,
	ComponentPool<MorphComponent>,
	ComponentPool<CharacterControllerComponent>,
	ComponentPool<FollowCameraComponent>
>;

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
	bool isActiveInHierarchy(Entity e) const;
	LightSource getLightSource(Entity e) const;
	void setLightSource(Entity e, LightSource source);
	bool isAnimated(Entity e) const;
	void setAnimated(Entity e, bool animated);

	// Component access
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

	// First T on `e` or any descendant (depth-first), or nullptr.
	template <typename T>
	T* getComponentInChildren(Entity e);

	// Generic pool accessor
	template <typename T>
	ComponentPool<T>& pool() { return std::get<ComponentPool<T>>(m_pools); }
	template <typename T>
	const ComponentPool<T>& pool() const { return std::get<ComponentPool<T>>(m_pools); }

	// Direct pool access (for system iteration)
	ComponentPool<TransformComponent>& transforms() { return pool<TransformComponent>(); }
	const ComponentPool<TransformComponent>& transforms() const { return pool<TransformComponent>(); }

	ComponentPool<MeshComponent>& meshes() { return pool<MeshComponent>(); }
	const ComponentPool<MeshComponent>& meshes() const { return pool<MeshComponent>(); }

	ComponentPool<PointLightComponent>& pointLights() { return pool<PointLightComponent>(); }
	const ComponentPool<PointLightComponent>& pointLights() const { return pool<PointLightComponent>(); }
	uint32_t activePointLightCount() const;

	ComponentPool<DirectionalLightComponent>& directionalLights() { return pool<DirectionalLightComponent>(); }
	const ComponentPool<DirectionalLightComponent>& directionalLights() const { return pool<DirectionalLightComponent>(); }
	uint32_t activeDirectionalLightCount() const;

	ComponentPool<SpotLightComponent>& spotLights() { return pool<SpotLightComponent>(); }
	const ComponentPool<SpotLightComponent>& spotLights() const { return pool<SpotLightComponent>(); }
	uint32_t activeSpotLightCount() const;

	ComponentPool<AreaLightComponent>& areaLights() { return pool<AreaLightComponent>(); }
	const ComponentPool<AreaLightComponent>& areaLights() const { return pool<AreaLightComponent>(); }
	uint32_t activeAreaLightCount() const;

	ComponentPool<RigidbodyComponent>& rigidbodies() { return pool<RigidbodyComponent>(); }
	const ComponentPool<RigidbodyComponent>& rigidbodies() const { return pool<RigidbodyComponent>(); }

	ComponentPool<CharacterControllerComponent>& characterControllers() { return pool<CharacterControllerComponent>(); }
	const ComponentPool<CharacterControllerComponent>& characterControllers() const { return pool<CharacterControllerComponent>(); }

	ComponentPool<AnimatorComponent>& animators() { return pool<AnimatorComponent>(); }
	const ComponentPool<AnimatorComponent>& animators() const { return pool<AnimatorComponent>(); }

	ComponentPool<SkinComponent>& skins() { return pool<SkinComponent>(); }
	const ComponentPool<SkinComponent>& skins() const { return pool<SkinComponent>(); }

	ComponentPool<CameraComponent>& cameras() { return pool<CameraComponent>(); }
	const ComponentPool<CameraComponent>& cameras() const { return pool<CameraComponent>(); }

	ComponentPool<FollowCameraComponent>& followCameras() { return pool<FollowCameraComponent>(); }
	const ComponentPool<FollowCameraComponent>& followCameras() const { return pool<FollowCameraComponent>(); }

	ComponentPool<ParticleEmitterComponent>& particleEmitters() { return pool<ParticleEmitterComponent>(); }
	const ComponentPool<ParticleEmitterComponent>& particleEmitters() const { return pool<ParticleEmitterComponent>(); }

	ComponentPool<MorphComponent>& morphs() { return pool<MorphComponent>(); }
	const ComponentPool<MorphComponent>& morphs() const { return pool<MorphComponent>(); }

	// Fast active check (skips generation validation)
	bool isActiveAtIndex(uint32_t index) const {
		return index < m_meta.size() && m_meta[index].active_in_hierarchy;
	}

	// Multi-component view factory
	template <typename... Components>
	auto view();

	// Hierarchy
	void   setParent(Entity child, Entity parent);
	void   reparent(Entity child, Entity new_parent);  // preserves world transform
	Entity getParent(Entity e) const;
	Entity firstChild(Entity e) const;
	Entity nextSibling(Entity e) const;
	bool   hasParent(Entity e) const;

	// World transforms
	const glm::mat4& getWorldTransform(Entity e) const;
	const glm::mat3& getWorldNormal(Entity e) const;
	glm::quat getWorldRotation(Entity e) const;
	// Writes a world-space pose onto e's local TransformComponent, undoing the parent
	// chain. Local scale is left alone. No-op if e has no TransformComponent.
	void setWorldPose(Entity e, const glm::vec3& world_pos, const glm::quat& world_rot);
	void invalidateWorldTransform(Entity e);
	// Ensures the world-transform cache entry for e is populated
	void primeWorldTransform(Entity e) const { (void)getWorldTransform(e); }

	// Cloning
	Entity cloneEntity(Entity source);
	Entity cloneEntityRecursive(Entity source);

	// Convenience factories
	Entity createGameObject(const std::string& name = "");
	Entity createPointLight(float intensity = 1.0f, float radius = 1.0f,
	                        glm::vec3 color = glm::vec3(1.0f));
	Entity createDirectionalLight(float intensity = 1.0f,
	                              glm::vec3 color = glm::vec3(1.0f),
	                              glm::vec3 direction = glm::vec3(0.f, -1.f, -1.f));
	Entity createSpotLight(float intensity = 1.0f, float radius = 1.0f,
	                       glm::vec3 color = glm::vec3(1.0f),
	                       glm::vec3 direction = glm::vec3(0.f, 0.f, -1.f),
	                       float inner_cone = glm::radians(25.0f),
	                       float outer_cone = glm::radians(35.0f));
	Entity createAreaLight(float intensity = 1.0f, glm::vec3 color = glm::vec3(1.0f),
	                       float width = 1.0f, float height = 1.0f);
	// Root-level camera entity (Transform + Camera + FollowCamera) orbiting `target`
	Entity createFollowCamera(Entity target, const std::string& name = "Follow Camera");

	void clear();

	// Deferred deletion
	bool hasPendingDeletions() const { return !m_pending_deletions.empty() || !m_pending_component_removals.empty(); }
	void processPendingDeletions();

	// Deferred component removal
	template <typename T>
	void queueComponentRemoval(Entity entity) {
		m_pending_component_removals.push_back({entity, [](Registry& reg, Entity e) {
			if (reg.hasComponent<T>(e))
				reg.removeComponent<T>(e);
		}});
	}

	// Entity for a given raw entity index (reconstructs with current generation)
	Entity entityFromIndex(uint32_t index) const;

	// Event dispatcher (scoped to this registry's lifetime)
	EcsEventDispatcher& events() { return m_events; }
	const EcsEventDispatcher& events() const { return m_events; }

private:
	void ensureSlotSize(uint32_t index);
	void invalidateMeshWorldAABBs(Entity e);
	void updateActiveInHierarchy(Entity e);
	void processPendingComponentRemovals();

	Entity cloneEntityCore(Entity source, bool reparent_to_source_parent);

	template <typename T>
	void cloneComponentIfPresent(Entity source, Entity clone) {
		if (auto* comp = getComponent<T>(source)) {
			T copy = *comp;  // copy before addComponent may reallocate the pool
			addComponent<T>(clone, std::move(copy));
		}
	}

	template <typename F>
	void forEachPool(F&& fn) {
		std::apply([&](auto&... pools) { (fn(pools), ...); }, m_pools);
	}

	template <typename F>
	void forEachPool(F&& fn) const {
		std::apply([&](const auto&... pools) { (fn(pools), ...); }, m_pools);
	}

	template <typename F>
	void forEachComponentType(F&& fn) {
		std::apply([&](auto&... pools) {
			(fn.template operator()<typename std::decay_t<decltype(pools)>::value_type>(), ...);
		}, m_pools);
	}

	// Entity management
	std::vector<EntityMeta> m_meta;
	std::vector<uint32_t> m_free_indices;
	uint32_t m_alive_count = 0;

	// Component pools
	ComponentPools m_pools;

	// Hierarchy (indexed by entity index)
	std::vector<HierarchyEntry> m_hierarchy;

	// World transform cache (indexed by entity index)
	mutable std::vector<WorldTransformCache> m_world_cache;

	EcsEventDispatcher m_events;
	std::vector<DeleteEntityRequest> m_pending_deletions;
	std::vector<PendingComponentRemoval> m_pending_component_removals;
};

// ── Template implementations ────────────────────────────────────────────────

template <typename T, typename... Args>
T& Registry::addComponent(Entity e, Args&&... args) {
	assert(isAlive(e) && "Entity is not alive");
	auto& comp = pool<T>().emplace(e.index(), std::forward<Args>(args)...);
	comp.setContext(e, this);
	m_events.emit(ComponentAddedEvent<T>{e, comp});
	return comp;
}

template <typename T>
void Registry::removeComponent(Entity e) {
	assert(isAlive(e) && "Entity is not alive");
	m_events.emit(ComponentRemovedEvent<T>{e});
	pool<T>().remove(e.index());
}

template <typename T>
T* Registry::getComponent(Entity e) {
	if (!isAlive(e)) return nullptr;
	return pool<T>().get(e.index());
}

template <typename T>
const T* Registry::getComponent(Entity e) const {
	if (!isAlive(e)) return nullptr;
	return pool<T>().get(e.index());
}

template <typename T>
bool Registry::hasComponent(Entity e) const {
	if (!isAlive(e)) return false;
	return pool<T>().has(e.index());
}

template <typename T>
T* Registry::getComponentInChildren(Entity e) {
	if (T* c = getComponent<T>(e))
		return c;
	for (Entity child = firstChild(e); !child.isNull(); child = nextSibling(child)) {
		if (T* c = getComponentInChildren<T>(child))
			return c;
	}
	return nullptr;
}

// Returns the subset of `items` whose ancestor chain contains no other item
VENGINE_API std::vector<Entity> topMostRoots(const Registry& registry, std::span<const Entity> items);

} // namespace ve

// View included after Registry is fully defined (View references Registry)
#include "ve_view.hpp"

namespace ve {
template <typename... Components>
auto Registry::view() {
	return View<Components...>(*this, pool<Components>()...);
}

// True if the entity's mesh is actually deformed at runtime
inline bool isDeformed(const Registry& registry, Entity entity) {
	const auto* mc = registry.getComponent<MeshComponent>(entity);
	const VeMesh* mesh = mc ? mc->getMesh() : nullptr;
	if (!mesh)
		return false;
	return (mesh->hasSkinning() && registry.hasComponent<SkinComponent>(entity))
	    || (mesh->hasMorphTargets() && registry.hasComponent<MorphComponent>(entity));
}
} // namespace ve
