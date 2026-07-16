#include "pch.hpp"
#include "input/character_input_driver.hpp"
#include "input/input_action.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace ve {

namespace {

void releaseCharacter(Registry& registry, Entity e) {
	if (e.isNull() || !registry.isAlive(e))
		return;
	if (auto* cc = registry.getComponent<CharacterControllerComponent>(e)) {
		cc->desired_velocity = glm::vec3(0.0f);
		cc->jump_requested = false;
	}
}

}

bool CharacterInputDriver::tick(Registry& registry, const InputActions& actions,
                                const glm::vec3& camera_forward, Entity& possessed, float dt) {
	const bool jump_pressed = actions.jump && !m_jump_was_down;
	m_jump_was_down = actions.jump;

	if (possessed != m_driven) {
		releaseCharacter(registry, m_driven);
		m_driven = possessed;
	}

	if (possessed.isNull())
		return false;

	auto* cc = registry.isAlive(possessed)
		? registry.getComponent<CharacterControllerComponent>(possessed)
		: nullptr;
	auto* tc = cc ? registry.getComponent<TransformComponent>(possessed) : nullptr;
	if (!cc || !tc) {
		possessed = Entity::null();
		m_driven = Entity::null();
		return false;
	}

	// Camera basis vectors
	glm::vec3 fwd{camera_forward.x, camera_forward.y, 0.0f};
	float fwd_len = glm::length(fwd);
	if (fwd_len > 1e-4f)
		fwd /= fwd_len;
	else
		fwd = glm::vec3(0.0f);
	glm::vec3 right = glm::cross(fwd, glm::vec3(0.0f, 0.0f, 1.0f));

	glm::vec3 input = fwd * actions.move_forward + right * actions.move_right;
	float input_len = glm::length(input);
	float target_speed = (actions.sprint ? cc->run_speed : cc->walk_speed) * std::min(input_len, 1.0f);

	// Ease speed toward the target; heading changes apply immediately
	float cur_speed = glm::length(glm::vec2(cc->desired_velocity));
	glm::vec3 heading{0.0f};
	if (input_len > 1e-4f)
		heading = input / input_len;
	else if (cur_speed > 1e-4f)
		heading = glm::vec3(cc->desired_velocity.x, cc->desired_velocity.y, 0.0f) / cur_speed;
	float max_delta = cc->acceleration * dt;
	float new_speed = cur_speed + glm::clamp(target_speed - cur_speed, -max_delta, max_delta);
	cc->desired_velocity = heading * new_speed;

	// Turn toward the movement heading at the configured rate
	if (input_len > 1e-4f) {
		float target_yaw = std::atan2(-heading.x, heading.y) + glm::radians(cc->facing_offset_deg);
		glm::quat target_rot = glm::angleAxis(target_yaw, glm::vec3(0.0f, 0.0f, 1.0f));

		// The heading is world-space but the component stores a local rotation
		Entity parent = registry.getParent(possessed);
		glm::quat parent_rot = parent.isNull() ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
		                                       : registry.getWorldRotation(parent);
		glm::quat cur_rot = parent_rot * tc->getRotation();

		float ang = glm::angle(target_rot * glm::inverse(cur_rot));
		if (ang > glm::pi<float>())
			ang = glm::two_pi<float>() - ang;
		if (ang > 1e-4f) {
			float t = std::min(1.0f, glm::radians(cc->turn_rate_deg) * dt / ang);
			glm::quat new_rot = glm::normalize(glm::slerp(cur_rot, target_rot, t));
			if (!parent.isNull())
				new_rot = glm::normalize(glm::inverse(parent_rot) * new_rot);
			tc->setRotation(new_rot);
		}
	}

	if (jump_pressed)
		cc->jump_requested = true;

	if (auto* anim = registry.getComponent<AnimatorComponent>(possessed)) {
		if (anim->hasBlendSpace()) {
			// Normalized gait: idle at 0, walk_speed maps to 0.5, run_speed to 1
			float speed = glm::length(glm::vec2(cc->velocity));
			float gait;
			if (speed <= cc->walk_speed)
				gait = cc->walk_speed > 1e-4f ? 0.5f * speed / cc->walk_speed : 0.0f;
			else {
				float span = cc->run_speed - cc->walk_speed;
				gait = span > 1e-4f ? 0.5f + 0.5f * (speed - cc->walk_speed) / span : 1.0f;
			}
			anim->setBlendParameter(glm::clamp(gait, 0.0f, 1.0f));
		}
	}

	return true;
}

}