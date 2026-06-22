#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"

#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace ve {

std::vector<Entity> topMostRoots(const Registry& registry, std::span<const Entity> items) {
	std::unordered_set<Entity> in_set(items.begin(), items.end());
	std::vector<Entity> roots;
	for (Entity e : items) {
		if (!registry.isAlive(e))
			continue;
		bool ancestor_in_set = false;
		for (Entity a = registry.getParent(e); !a.isNull(); a = registry.getParent(a))
			if (in_set.count(a)) {
				ancestor_in_set = true;
				break;
			}
		if (!ancestor_in_set)
			roots.push_back(e);
	}
	return roots;
}

Registry::Registry() {
	m_events.subscribe<DeleteEntityRequest>([this](const DeleteEntityRequest& req) {
		m_pending_deletions.push_back(req);
	});
}

Registry::~Registry() = default;

// ── Entity lifecycle ────────────────────────────────────────────────────────

Entity Registry::createEntity(const std::string& name) {
	uint32_t index;
	if (!m_free_indices.empty()) {
		index = m_free_indices.back();
		m_free_indices.pop_back();
	} else {
		index = static_cast<uint32_t>(m_meta.size());
		m_meta.emplace_back();
		m_hierarchy.emplace_back();
		m_world_cache.emplace_back();
	}
	auto& meta = m_meta[index];
	meta.name = name;
	meta.active = true;
	meta.active_in_hierarchy = true;
	meta.alive = true;
	// generation was already incremented on destroy (or is 0 for fresh slots)

	m_alive_count++;
	Entity e(index, meta.generation);
	m_events.emit(EntityCreatedEvent{e});
	return e;
}

void Registry::destroyEntity(Entity e) {
	assert(isAlive(e) && "Destroying dead entity");
	uint32_t idx = e.index();

	// Emit events before teardown so subscribers can still inspect the entity
	m_events.emit(EntityDestroyedEvent{e});
	forEachPool([&](auto& p) {
		using Comp = typename std::decay_t<decltype(p)>::value_type;
		if (p.has(idx))
			m_events.emit(ComponentRemovedEvent<Comp>{e});
	});

	// Remove all components
	forEachPool([&](auto& p) {
		if (p.has(idx))
			p.remove(idx);
	});

	// Detach from hierarchy: unlink from parent's child list
	auto& h = m_hierarchy[idx];
	if (!h.parent.isNull() && isAlive(h.parent)) {
		uint32_t parent_idx = h.parent.index();
		auto& ph = m_hierarchy[parent_idx];
		// Walk the sibling list and remove ourselves
		if (!ph.first_child.isNull() && ph.first_child.index() == idx) {
			ph.first_child = h.next_sibling;
		} else {
			Entity prev = ph.first_child;
			while (!prev.isNull()) {
				auto& prev_h = m_hierarchy[prev.index()];
				if (!prev_h.next_sibling.isNull() && prev_h.next_sibling.index() == idx) {
					prev_h.next_sibling = h.next_sibling;
					break;
				}
				prev = prev_h.next_sibling;
			}
		}
	}

	// Orphan all children; recompute active_in_hierarchy now their parent link is
	// gone 
	Entity child = h.first_child;
	while (!child.isNull()) {
		Entity next = m_hierarchy[child.index()].next_sibling;
		m_hierarchy[child.index()].parent = Entity::null();
		updateActiveInHierarchy(child);
		child = next;
	}

	h.parent = Entity::null();
	h.first_child = Entity::null();
	h.next_sibling = Entity::null();

	// Reset world cache
	m_world_cache[idx] = WorldTransformCache{};

	// Increment generation to invalidate outstanding Entity handles
	m_meta[idx].generation = static_cast<uint16_t>((m_meta[idx].generation + 1) & Entity::GEN_MASK);
	m_meta[idx].active = false;
	m_meta[idx].active_in_hierarchy = false;
	m_meta[idx].alive = false;
	m_meta[idx].name.clear();

	m_free_indices.push_back(idx);
	m_alive_count--;
}

void Registry::destroyEntityRecursive(Entity e) {
	if (!isAlive(e))
		return;

	// Collect entire subtree first
	std::vector<Entity> to_destroy;
	std::vector<Entity> stack = {e};
	while (!stack.empty()) {
		Entity cur = stack.back();
		stack.pop_back();
		to_destroy.push_back(cur);
		Entity child = firstChild(cur);
		while (!child.isNull()) {
			stack.push_back(child);
			child = nextSibling(child);
		}
	}

	// Destroy leaves first
	for (auto it = to_destroy.rbegin(); it != to_destroy.rend(); ++it)
		destroyEntity(*it);
}

void Registry::processPendingDeletions() {
	// Process pending component removals first
	processPendingComponentRemovals();

	if (m_pending_deletions.empty())
		return;
	// Move to local to allow re-entrant emit during destruction
	auto deletions = std::move(m_pending_deletions);
	m_pending_deletions.clear();
	for (auto& req : deletions) {
		if (!isAlive(req.entity))
			continue;
		if (req.recursive)
			destroyEntityRecursive(req.entity);
		else
			destroyEntity(req.entity);
	}
}

void Registry::processPendingComponentRemovals() {
	if (m_pending_component_removals.empty())
		return;
	auto removals = std::move(m_pending_component_removals);
	m_pending_component_removals.clear();
	for (auto& r : removals) {
		if (!isAlive(r.entity))
			continue;
		r.remove_fn(*this, r.entity);
	}
}

bool Registry::isAlive(Entity e) const {
	if (e.isNull())
		return false;
	uint32_t idx = e.index();
	if (idx >= m_meta.size())
		return false;
	return m_meta[idx].generation == e.generation();
}

bool Registry::isAliveAtIndex(uint32_t index) const {
	if (index >= m_meta.size())
		return false;
	return m_meta[index].alive;
}

// ── Entity metadata ─────────────────────────────────────────────────────────

const std::string& Registry::getName(Entity e) const {
	assert(isAlive(e));
	return m_meta[e.index()].name;
}

void Registry::setName(Entity e, std::string name) {
	assert(isAlive(e));
	m_meta[e.index()].name = std::move(name);
}

bool Registry::isActive(Entity e) const {
	if (!isAlive(e))
		return false;
	return m_meta[e.index()].active;
}

void Registry::setActive(Entity e, bool active) {
	assert(isAlive(e));
	uint32_t idx = e.index();
	if (m_meta[idx].active == active)
		return;
	m_meta[idx].active = active;
	updateActiveInHierarchy(e);
}

bool Registry::isActiveInHierarchy(Entity e) const {
	if (!isAlive(e))
		return false;
	return m_meta[e.index()].active_in_hierarchy;
}

uint32_t Registry::activePointLightCount() const {
	uint32_t count = 0;
	auto& p = pool<PointLightComponent>();
	for (uint32_t i = 0; i < p.size(); ++i)
		if (m_meta[p.entityAt(i)].active_in_hierarchy)
			++count;
	return count;
}

uint32_t Registry::activeDirectionalLightCount() const {
	uint32_t count = 0;
	auto& p = pool<DirectionalLightComponent>();
	for (uint32_t i = 0; i < p.size(); ++i)
		if (m_meta[p.entityAt(i)].active_in_hierarchy)
			++count;
	return count;
}

uint32_t Registry::activeSpotLightCount() const {
	uint32_t count = 0;
	auto& p = pool<SpotLightComponent>();
	for (uint32_t i = 0; i < p.size(); ++i)
		if (m_meta[p.entityAt(i)].active_in_hierarchy)
			++count;
	return count;
}

LightSource Registry::getLightSource(Entity e) const {
	assert(isAlive(e));
	return m_meta[e.index()].light_source;
}

void Registry::setLightSource(Entity e, LightSource source) {
	assert(isAlive(e));
	m_meta[e.index()].light_source = source;
}

bool Registry::isAnimated(Entity e) const {
	if (!isAlive(e)) return false;
	return m_meta[e.index()].animated;
}

void Registry::setAnimated(Entity e, bool animated) {
	assert(isAlive(e));
	m_meta[e.index()].animated = animated;
}

// ── Hierarchy ───────────────────────────────────────────────────────────────

void Registry::setParent(Entity child, Entity parent) {
	assert(isAlive(child));
	uint32_t child_idx = child.index();
	auto& ch = m_hierarchy[child_idx];

	// Already this parent?
	if (ch.parent == parent) return;

	// Unlink from current parent
	if (!ch.parent.isNull() && isAlive(ch.parent)) {
		uint32_t old_parent_idx = ch.parent.index();
		auto& ph = m_hierarchy[old_parent_idx];
		if (!ph.first_child.isNull() && ph.first_child.index() == child_idx) {
			ph.first_child = ch.next_sibling;
		} else {
			Entity prev = ph.first_child;
			while (!prev.isNull()) {
				auto& prev_h = m_hierarchy[prev.index()];
				if (!prev_h.next_sibling.isNull() && prev_h.next_sibling.index() == child_idx) {
					prev_h.next_sibling = ch.next_sibling;
					break;
				}
				prev = prev_h.next_sibling;
			}
		}
	}

	ch.next_sibling = Entity::null();
	ch.parent = parent;

	// Link to new parent
	if (!parent.isNull() && isAlive(parent)) {
		uint32_t parent_idx = parent.index();
		auto& ph = m_hierarchy[parent_idx];
		ch.next_sibling = ph.first_child;
		ph.first_child = child;
	}

	invalidateWorldTransform(child);
	updateActiveInHierarchy(child);
}

void Registry::reparent(Entity child, Entity new_parent) {
	assert(isAlive(child));
	auto* tc = getComponent<TransformComponent>(child);
	if (!tc) {
		setParent(child, new_parent);
		return;
	}

	// Capture current world transform before reparenting
	const glm::mat4 old_world = getWorldTransform(child);

	setParent(child, new_parent);

	// Compute new local transform that preserves the old world position
	glm::mat4 new_local = old_world;
	if (!new_parent.isNull() && isAlive(new_parent))
		new_local = glm::inverse(getWorldTransform(new_parent)) * old_world;

	glm::vec3 translation, scale, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(new_local, scale, rotation, translation, skew, perspective);

	tc->setTranslation(translation);
	tc->setRotation(rotation);
	tc->setScale(scale);
}

Entity Registry::getParent(Entity e) const {
	if (e.isNull() || e.index() >= m_hierarchy.size())
		return Entity::null();
	return m_hierarchy[e.index()].parent;
}

Entity Registry::firstChild(Entity e) const {
	if (e.isNull() || e.index() >= m_hierarchy.size())
		return Entity::null();
	return m_hierarchy[e.index()].first_child;
}

Entity Registry::nextSibling(Entity e) const {
	if (e.isNull() || e.index() >= m_hierarchy.size())
		return Entity::null();
	return m_hierarchy[e.index()].next_sibling;
}

bool Registry::hasParent(Entity e) const {
	if (e.isNull() || e.index() >= m_hierarchy.size())
		return false;
	return !m_hierarchy[e.index()].parent.isNull();
}

// ── World transforms ────────────────────────────────────────────────────────

const glm::mat4& Registry::getWorldTransform(Entity e) const {
	assert(isAlive(e));
	uint32_t idx = e.index();
	auto& cache = m_world_cache[idx];

	if (cache.transform_dirty) {
		const auto* tc = pool<TransformComponent>().get(idx);
		assert(tc && "Entity must have TransformComponent for world transform");
		const glm::mat4& local = tc->getTransform();

		Entity parent = m_hierarchy[idx].parent;
		if (!parent.isNull() && isAlive(parent)) {
			cache.world_transform = getWorldTransform(parent) * local;
		} else {
			cache.world_transform = local;
		}
		cache.transform_dirty = false;
		cache.normal_dirty = true;
	}
	return cache.world_transform;
}

const glm::mat3& Registry::getWorldNormal(Entity e) const {
	assert(isAlive(e));
	uint32_t idx = e.index();
	auto& cache = m_world_cache[idx];

	if (cache.normal_dirty) {
		Entity parent = m_hierarchy[idx].parent;
		if (!parent.isNull() && isAlive(parent)) {
			const glm::mat4& world = getWorldTransform(e);
			cache.world_normal = glm::mat3(glm::inverse(glm::transpose(world)));
		} else {
			const auto* tc = pool<TransformComponent>().get(idx);
			assert(tc);
			cache.world_normal = tc->getNormalTransform();
		}
		cache.normal_dirty = false;
	}
	return cache.world_normal;
}

void Registry::invalidateWorldTransform(Entity e) {
	if (e.isNull() || e.index() >= m_world_cache.size())
		return;
	uint32_t idx = e.index();
	auto& cache = m_world_cache[idx];
	cache.transform_dirty = true;
	cache.normal_dirty = true;

	// Invalidate mesh AABB
	invalidateMeshWorldAABBs(e);

	m_events.emit(TransformInvalidatedEvent{e});

	// Cascade to children
	Entity child = m_hierarchy[idx].first_child;
	while (!child.isNull()) {
		invalidateWorldTransform(child);
		child = m_hierarchy[child.index()].next_sibling;
	}
}

void Registry::updateActiveInHierarchy(Entity e) {
	if (e.isNull() || e.index() >= m_meta.size())
		return;
	uint32_t idx = e.index();
	Entity parent = m_hierarchy[idx].parent;
	bool parent_active = parent.isNull() || m_meta[parent.index()].active_in_hierarchy;
	bool effective = m_meta[idx].active && parent_active;
	if (m_meta[idx].active_in_hierarchy == effective)
		return;
	m_meta[idx].active_in_hierarchy = effective;
	m_events.emit(ActiveChangedEvent{e, effective});

	Entity child = m_hierarchy[idx].first_child;
	while (!child.isNull()) {
		updateActiveInHierarchy(child);
		child = m_hierarchy[child.index()].next_sibling;
	}
}

void Registry::invalidateMeshWorldAABBs(Entity e) {
	uint32_t idx = e.index();
	auto* mesh = pool<MeshComponent>().get(idx);
	if (mesh) {
		mesh->invalidateWorldAABB();
	}
}

// ── Cloning ─────────────────────────────────────────────────────────────────

Entity Registry::cloneEntityCore(Entity source, bool reparent_to_source_parent) {
	assert(isAlive(source) && "Cannot clone dead entity");
	uint32_t src_idx = source.index();

	Entity clone = createEntity(m_meta[src_idx].name + " (Copy)");

	m_meta[clone.index()].light_source = m_meta[src_idx].light_source;

	forEachComponentType([&]<typename T>() {
		cloneComponentIfPresent<T>(source, clone);
	});

	if (!m_meta[src_idx].active)
		setActive(clone, false);

	if (reparent_to_source_parent) {
		Entity parent = m_hierarchy[src_idx].parent;
		if (!parent.isNull() && isAlive(parent))
			setParent(clone, parent);
	}

	return clone;
}

Entity Registry::cloneEntity(Entity source) {
	return cloneEntityCore(source, /*reparent_to_source_parent=*/true);
}

Entity Registry::cloneEntityRecursive(Entity source) {
	assert(isAlive(source) && "Cannot clone dead entity");

	// BFS over source subtree, cloning each entity with correct parent links.
	// Events are suppressed during cloning
	struct CloneEntry {
		Entity source;
		Entity clone_parent;
	};
	std::vector<CloneEntry> queue;
	queue.push_back({source, m_hierarchy[source.index()].parent});

	Entity root_clone = Entity::null();
	std::vector<Entity> cloned_entities;
	std::unordered_map<uint32_t, Entity> old_to_new;

	m_events.beginBatch();

	for (size_t i = 0; i < queue.size(); ++i) {
		auto [src, clone_parent] = queue[i];
		// Root attaches to the source's parent; descendants skip that and
		// are parented to their corresponding clone parent below.
		Entity clone = cloneEntityCore(src, /*reparent_to_source_parent=*/i == 0);
		cloned_entities.push_back(clone);
		old_to_new[src.index()] = clone;

		if (i == 0)
			root_clone = clone;
		else
			setParent(clone, clone_parent);

		Entity child = firstChild(src);
		while (!child.isNull()) {
			queue.push_back({child, clone});
			child = nextSibling(child);
		}
	}

	// Remap cross-entity references so clones drive their own joints/targets.
	// Hand-listed: AnimatorComponent and SkinComponent are the only components
	// that store Entity references today.
	for (Entity clone : cloned_entities) {
		if (auto* anim = getComponent<AnimatorComponent>(clone))
			anim->remapEntities(old_to_new);
		if (auto* skin = getComponent<SkinComponent>(clone))
			skin->remapEntities(old_to_new);
	}

	m_events.endBatch();

	// Replay ComponentAddedEvents now that the full hierarchy exists
	for (Entity clone : cloned_entities) {
		forEachComponentType([&]<typename T>() {
			if (auto* comp = getComponent<T>(clone))
				m_events.emit(ComponentAddedEvent<T>{clone, *comp});
		});
	}

	return root_clone;
}

// ── Convenience factories ───────────────────────────────────────────────────

Entity Registry::createGameObject(const std::string& name) {
	Entity e = createEntity(name);
	addComponent<TransformComponent>(e);
	return e;
}

Entity Registry::createPointLight(float intensity, float radius, glm::vec3 color) {
	Entity e = createGameObject();
	m_events.beginBatch();
	auto& pl = addComponent<PointLightComponent>(e);
	pl.setIntensity(intensity);
	pl.setColor(color);
	m_events.endBatch();

	auto* transform = getComponent<TransformComponent>(e);
	transform->setScale(glm::vec3(radius));
	return e;
}

Entity Registry::createDirectionalLight(float intensity, glm::vec3 color, glm::vec3 direction) {
	Entity e = createEntity();
	m_events.beginBatch();
	auto& dl = addComponent<DirectionalLightComponent>(e);
	dl.setIntensity(intensity);
	dl.setColor(color);
	dl.setDirection(glm::normalize(direction));
	dl.setCastsShadow(false);
	m_events.endBatch();
	return e;
}

Entity Registry::createSpotLight(float intensity, float radius, glm::vec3 color,
                                 glm::vec3 direction, float inner_cone, float outer_cone) {
	Entity e = createGameObject();
	m_events.beginBatch();
	auto& sl = addComponent<SpotLightComponent>(e);
	sl.setIntensity(intensity);
	sl.setColor(color);
	sl.setDirection(direction);
	sl.setInnerConeAngle(inner_cone);
	sl.setOuterConeAngle(outer_cone);
	m_events.endBatch();
	auto* transform = getComponent<TransformComponent>(e);
	transform->setScale(glm::vec3(radius));
	return e;
}

// ── Bulk operations ─────────────────────────────────────────────────────────

void Registry::clear() {
	forEachPool([](auto& p) { p.clear(); });
	m_meta.clear();
	m_hierarchy.clear();
	m_world_cache.clear();
	m_free_indices.clear();
	m_alive_count = 0;
}

Entity Registry::entityFromIndex(uint32_t index) const {
	assert(index < m_meta.size());
	return Entity(index, m_meta[index].generation);
}

void Registry::ensureSlotSize(uint32_t index) {
	if (index >= m_meta.size()) {
		m_meta.resize(index + 1);
		m_hierarchy.resize(index + 1);
		m_world_cache.resize(index + 1);
	}
}

} // namespace ve
