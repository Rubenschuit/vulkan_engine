#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cassert>

namespace ve {

Registry::Registry() = default;
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
	// generation was already incremented on destroy (or is 0 for fresh slots)

	m_alive_count++;
	return Entity(index, meta.generation);
}

void Registry::destroyEntity(Entity e) {
	assert(isAlive(e) && "Destroying dead entity");
	uint32_t idx = e.index();

	// Adjust active light counters before removing components
	if (m_meta[idx].active) {
		if (m_point_lights.has(idx))
			m_active_point_lights--;
		if (m_directional_lights.has(idx))
			m_active_directional_lights--;
	}

	// Remove all components
	if (m_transforms.has(idx))
		m_transforms.remove(idx);
	if (m_meshes.has(idx))
		m_meshes.remove(idx);
	if (m_point_lights.has(idx))
		m_point_lights.remove(idx);
	if (m_directional_lights.has(idx))
		m_directional_lights.remove(idx);

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

	// Orphan all children
	Entity child = h.first_child;
	while (!child.isNull()) {
		auto& ch = m_hierarchy[child.index()];
		ch.parent = Entity::null();
		child = ch.next_sibling;
	}

	h.parent = Entity::null();
	h.first_child = Entity::null();
	h.next_sibling = Entity::null();

	// Reset world cache
	m_world_cache[idx] = WorldTransformCache{};

	// Increment generation to invalidate outstanding Entity handles
	m_meta[idx].generation = static_cast<uint16_t>((m_meta[idx].generation + 1) & Entity::GEN_MASK);
	m_meta[idx].active = false;
	m_meta[idx].name.clear();

	m_free_indices.push_back(idx);
	m_alive_count--;
}

bool Registry::isAlive(Entity e) const {
	if (e.isNull())
		return false;
	uint32_t idx = e.index();
	if (idx >= m_meta.size())
		return false;
	return m_meta[idx].generation == e.generation();
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
	bool was_active = m_meta[idx].active;
	m_meta[idx].active = active;
	if (was_active != active) {
		if (m_point_lights.has(idx))
			active ? m_active_point_lights++ : m_active_point_lights--;
		else if (m_directional_lights.has(idx))
			active ? m_active_directional_lights++ : m_active_directional_lights--;
	}
}

uint32_t Registry::activePointLightCount() const { return m_active_point_lights; }
uint32_t Registry::activeDirectionalLightCount() const { return m_active_directional_lights; }

LightSource Registry::getLightSource(Entity e) const {
	assert(isAlive(e));
	return m_meta[e.index()].light_source;
}

void Registry::setLightSource(Entity e, LightSource source) {
	assert(isAlive(e));
	m_meta[e.index()].light_source = source;
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
		const auto* tc = m_transforms.get(idx);
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
			const auto* tc = m_transforms.get(idx);
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

	// Cascade to children
	Entity child = m_hierarchy[idx].first_child;
	while (!child.isNull()) {
		invalidateWorldTransform(child);
		child = m_hierarchy[child.index()].next_sibling;
	}
}

void Registry::invalidateMeshWorldAABBs(Entity e) {
	uint32_t idx = e.index();
	auto* mesh = m_meshes.get(idx);
	if (mesh) {
		mesh->invalidateWorldAABB();
	}
}

// ── Convenience factories ───────────────────────────────────────────────────

Entity Registry::createGameObject(const std::string& name) {
	Entity e = createEntity(name);
	addComponent<TransformComponent>(e);
	return e;
}

Entity Registry::createPointLight(float intensity, float radius, glm::vec3 color) {
	Entity e = createGameObject();
	auto& pl = addComponent<PointLightComponent>(e);
	pl.setIntensity(intensity);
	pl.setColor(color);

	auto* transform = getComponent<TransformComponent>(e);
	transform->setScale(glm::vec3(radius));
	return e;
}

Entity Registry::createDirectionalLight(float intensity, glm::vec3 color, glm::vec3 direction) {
	Entity e = createGameObject();
	auto& dl = addComponent<DirectionalLightComponent>(e);
	dl.intensity = intensity;
	dl.color = color;
	dl.direction = glm::normalize(direction);
	dl.casts_shadow = false;
	return e;
}

// ── Bulk operations ─────────────────────────────────────────────────────────

void Registry::clear() {
	m_transforms.clear();
	m_meshes.clear();
	m_point_lights.clear();
	m_directional_lights.clear();
	m_meta.clear();
	m_hierarchy.clear();
	m_world_cache.clear();
	m_free_indices.clear();
	m_alive_count = 0;
	m_active_point_lights = 0;
	m_active_directional_lights = 0;
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
