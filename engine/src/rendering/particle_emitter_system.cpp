#include "pch.hpp"
#include "rendering/particle_emitter_system.hpp"
#include "rendering/particle_backend.hpp"
#include "events/engine_events.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_scene.hpp"
#include "scene/ve_view.hpp"

namespace ve {

ParticleEmitterSystem::ParticleEmitterSystem(ParticleBackend& backend, EventBus& event_bus)
	: m_backend(backend), m_event_bus(event_bus) {
	m_scene_loaded_sub = m_event_bus.subscribe<SceneLoadedEvent>(
		[this](const SceneLoadedEvent& e) { onSceneLoaded(e); });
	m_scene_unloaded_sub = m_event_bus.subscribe<SceneUnloadedEvent>(
		[this](const SceneUnloadedEvent& e) { onSceneUnloaded(e); });
}

ParticleEmitterSystem::~ParticleEmitterSystem() {
	m_event_bus.unsubscribe<SceneLoadedEvent>(m_scene_loaded_sub);
	m_event_bus.unsubscribe<SceneUnloadedEvent>(m_scene_unloaded_sub);
	if (m_active_registry) {
		m_active_registry->events().unsubscribe<ComponentAddedEvent<ParticleEmitterComponent>>(m_add_sub);
		m_active_registry->events().unsubscribe<ComponentRemovedEvent<ParticleEmitterComponent>>(m_remove_sub);
	}
	releaseAllSlots();
}

void ParticleEmitterSystem::releaseAllSlots() {
	for (auto& [_, id] : m_slot_for)
		m_backend.releaseEmitter(id);
	m_slot_for.clear();
	for (auto& [_, ref] : m_texture_slots)
		m_backend.releaseAtlas(ref.slot);
	m_texture_slots.clear();
}

void ParticleEmitterSystem::onSceneUnloaded(const SceneUnloadedEvent& /*e*/) {
	if (m_active_registry) {
		m_active_registry->events().unsubscribe<ComponentAddedEvent<ParticleEmitterComponent>>(m_add_sub);
		m_active_registry->events().unsubscribe<ComponentRemovedEvent<ParticleEmitterComponent>>(m_remove_sub);
	}
	releaseAllSlots();
	m_active_registry = nullptr;
	m_add_sub = 0;
	m_remove_sub = 0;
}

void ParticleEmitterSystem::onSceneLoaded(const SceneLoadedEvent& e) {
	if (!e.registry)
		return;
	m_active_registry = e.registry;

	m_add_sub = m_active_registry->events().subscribe<ComponentAddedEvent<ParticleEmitterComponent>>(
		[this](const ComponentAddedEvent<ParticleEmitterComponent>& ev) {
			onEmitterAdded(ev.entity, ev.component);
		});
	m_remove_sub = m_active_registry->events().subscribe<ComponentRemovedEvent<ParticleEmitterComponent>>(
		[this](const ComponentRemovedEvent<ParticleEmitterComponent>& ev) {
			onEmitterRemoved(ev.entity);
		});

	for (auto [entity, emitter] : m_active_registry->view<ParticleEmitterComponent>())
		onEmitterAdded(entity, emitter);
}

void ParticleEmitterSystem::onEmitterAdded(Entity entity, ParticleEmitterComponent& c) {
	if (m_slot_for.find(entity) != m_slot_for.end())
		return;
	if (c.texture.isValid()) {
		auto [it, inserted] = m_texture_slots.try_emplace(c.texture.getId(), TextureRef{});
		if (inserted)
			it->second.slot = m_backend.registerAtlas(c.texture);
		it->second.refcount++;
		c.params.atlas_index = it->second.slot;
	}
	EmitterId id = m_backend.registerEmitter(c.params);
	if (id == INVALID_EMITTER)
		return;
	m_slot_for.emplace(entity, id);
}

void ParticleEmitterSystem::onEmitterRemoved(Entity entity) {
	auto it = m_slot_for.find(entity);
	if (it != m_slot_for.end()) {
		m_backend.releaseEmitter(it->second);
		m_slot_for.erase(it);
	}
	// Drop the texture ref if the component still carries one.
	if (!m_active_registry)
		return;
	auto* c = m_active_registry->getComponent<ParticleEmitterComponent>(entity);
	if (!c || !c->texture.isValid())
		return;
	auto slot_it = m_texture_slots.find(c->texture.getId());
	if (slot_it != m_texture_slots.end() && --slot_it->second.refcount == 0) {
		m_backend.releaseAtlas(slot_it->second.slot);
		m_texture_slots.erase(slot_it);
	}
}

void ParticleEmitterSystem::tick(Registry& registry, float dt) {
	for (auto [entity, transform, emitter] : registry.view<TransformComponent, ParticleEmitterComponent>()) {
		auto it = m_slot_for.find(entity);
		if (it == m_slot_for.end())
			continue;
		if (!emitter.isActive())
			continue;
		const EmitterId id = it->second;

		glm::vec3 world_pos = glm::vec3(registry.getWorldTransform(entity)[3]);
		emitter.params.origin = glm::vec4(world_pos, 1.0f);
		m_backend.updateEmitter(id, emitter.params);

		if (emitter.rate > 0.0f) {
			emitter.rate_accumulator += dt * emitter.rate;
			uint32_t to_emit = static_cast<uint32_t>(emitter.rate_accumulator);
			emitter.rate_accumulator -= static_cast<float>(to_emit);
			if (to_emit > 0) {
				SpawnEvent e{
					.position_scale = glm::vec4(world_pos, emitter.scale),
					.velocity_life = glm::vec4(0.0f),
					.color = emitter.params.color_start,
					.count = to_emit,
					.emitter_id = id,
				};
				m_backend.emitParticles(e);
			}
		}

		if (emitter.burst_count > 0 && emitter.burst_period >= 1.0e-4f) {
			emitter.burst_accumulator -= dt;
			constexpr int MAX_BURSTS_PER_TICK = 32;
			int fired = 0;
			while (emitter.burst_accumulator <= 0.0f && fired < MAX_BURSTS_PER_TICK) {
				emitter.burst_accumulator += emitter.burst_period;
				SpawnEvent e{
					.position_scale = glm::vec4(world_pos, emitter.scale),
					.velocity_life = glm::vec4(0.0f),
					.color = emitter.params.color_start,
					.count = emitter.burst_count,
					.emitter_id = id,
				};
				m_backend.emitParticles(e);
				++fired;
			}
			if (emitter.burst_accumulator < 0.0f)
				emitter.burst_accumulator = 0.0f;
		}
	}
}

} // namespace ve