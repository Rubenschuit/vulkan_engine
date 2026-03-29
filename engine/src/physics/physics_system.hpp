#pragma once
#include "ve_export.hpp"
#include "scene/ve_entity.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <vector>
#include <cstdint>
#include <optional>

namespace ve {

class EventBus;
class Registry;

enum class DebugShapeType : uint8_t { Box, Sphere, Capsule, ConvexHull, Compound };

struct DebugShape {
	DebugShapeType type;
	glm::vec3 position;
	glm::quat rotation;
	glm::vec3 extents; // box: half-extents, sphere: x=radius, capsule: x=radius y=half_height
	glm::vec3 offset;  // local shape offset (from RotatedTranslatedShape)
	bool is_dynamic;
	std::vector<glm::vec3> hull_vertices;
	std::vector<std::pair<uint32_t, uint32_t>> hull_edges;
	std::vector<DebugShape> sub_shapes; // compound sub-shapes
};

struct PhysicsConfig {
	float fixed_timestep = 1.0f / 60.0f;
	uint32_t max_substeps = 4;
	uint32_t max_bodies = 65536;
	uint32_t max_body_pairs = 65536;
	uint32_t max_contact_constraints = 10240;
	glm::vec3 gravity = {0.0f, 0.0f, -9.81f};
};

class VENGINE_API PhysicsSystem {
public:
	explicit PhysicsSystem(const PhysicsConfig& config = {});
	~PhysicsSystem();

	PhysicsSystem(const PhysicsSystem&) = delete;
	PhysicsSystem& operator=(const PhysicsSystem&) = delete;

	void onSceneLoaded(Registry& registry);
	void onSceneUnloaded();

	void update(float dt, Registry& registry);

	void addStaticCollidersForAllMeshes(Registry& registry);

	void freezeBody(Entity entity);
	void unfreezeBody(Entity entity);
	void setPreserveVelocity(Entity entity, bool preserve);

	void setEventBus(EventBus* bus);

	uint32_t getActiveBodyCount() const;
	std::optional<DebugShape> getDebugShape(Entity entity, Registry& registry) const;

private:
	// Opaque pointer to implementation to avoid heavy includes in the header
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace ve
