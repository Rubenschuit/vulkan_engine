/* CharacterInputDriver - drives a possessed character entity from input:
 * camera-relative desired velocity, yaw toward the movement heading, and the
 * locomotion blend parameter. 
 */
#pragma once
#include "ve_export.hpp"
#include "scene/ve_entity.hpp"
#include <glm/glm.hpp>

namespace ve {

class Registry;
struct InputActions;

class VENGINE_API CharacterInputDriver {
public:
	// Returns true when movement input was consumed. Clears `possessed` if the
	// entity is no longer valid, and zeroes the movement state of a character it
	// stops driving.
	bool tick(Registry& registry, const InputActions& actions,
	          const glm::vec3& camera_forward, Entity& possessed, float dt);

private:
	Entity m_driven = Entity::null();
	bool m_jump_was_down = false;
	float m_air_time = 0.0f;
	bool m_air_posed = false;
	float m_air_pitch = 0.0f; // current lean, rad
	bool m_was_pitched = false;
};

} 