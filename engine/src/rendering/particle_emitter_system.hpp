#pragma once
#include "ve_export.hpp"
#include "scene/ecs_event_dispatcher.hpp"
#include "scene/ve_entity.hpp"
#include "events/event_bus.hpp"
#include "rendering/particle_emitter_params.hpp"

#include <string>
#include <unordered_map>

namespace ve {

class ParticleBackend;
class Registry;
class ParticleEmitterComponent;
struct SceneLoadedEvent;
struct SceneUnloadedEvent;

// Drives ECS ParticleEmitterComponents against the shared ParticleBackend.
class VENGINE_API ParticleEmitterSystem {
public:
	ParticleEmitterSystem(ParticleBackend& backend, EventBus& event_bus);
	~ParticleEmitterSystem();

	ParticleEmitterSystem(const ParticleEmitterSystem&) = delete;
	ParticleEmitterSystem& operator=(const ParticleEmitterSystem&) = delete;

	// Per-frame: push transforms/params into the backend SSBO and emit
	// rate/burst SpawnEvents.
	void tick(Registry& registry, float dt);

private:
	void onSceneLoaded(const SceneLoadedEvent& e);
	void onSceneUnloaded(const SceneUnloadedEvent& e);
	void onEmitterAdded(Entity entity, ParticleEmitterComponent& c);
	void onEmitterRemoved(Entity entity);
	void releaseAllSlots();

	ParticleBackend& m_backend;
	EventBus& m_event_bus;

	Registry* m_active_registry = nullptr;
	std::unordered_map<Entity, EmitterId> m_slot_for;

	struct TextureRef { uint32_t slot; uint32_t refcount; };
	std::unordered_map<std::string, TextureRef> m_texture_slots;

	EventSubscriptionId m_scene_loaded_sub = 0;
	EventSubscriptionId m_scene_unloaded_sub = 0;
	SubscriptionId m_add_sub = 0;
	SubscriptionId m_remove_sub = 0;
};

} // namespace ve