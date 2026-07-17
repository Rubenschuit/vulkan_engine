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

constexpr float AIR_POSE_DELAY = 0.1f;
constexpr float AIR_POSE_FADE_IN = 0.1f;
constexpr float AIR_POSE_FADE_OUT = 0.15f;
const float AIR_PITCH_RATE = glm::radians(120.0f); // lean approach speed, rad/s

glm::quat twistZ(const glm::quat& q) {
	float len2 = q.w * q.w + q.z * q.z;
	if (len2 < 1e-12f)
		return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	float inv = 1.0f / std::sqrt(len2);
	return glm::quat(q.w * inv, 0.0f, 0.0f, q.z * inv);
}

void releaseCharacter(Registry& registry, Entity e, bool air_posed) {
	if (e.isNull() || !registry.isAlive(e))
		return;
	if (auto* cc = registry.getComponent<CharacterControllerComponent>(e)) {
		cc->desired_velocity = glm::vec3(0.0f);
		cc->jump_requested = false;
	}
	if (air_posed) {
		if (auto* anim = registry.getComponent<AnimatorComponent>(e)) {
			if (anim->getAirPoseClip() >= 0)
				anim->fadeClipWeight(static_cast<uint32_t>(anim->getAirPoseClip()), 0.0f, AIR_POSE_FADE_OUT);
			anim->setBlendParameter(anim->getBlendParameter()); // resume the gait
		}
	}
}

}

bool CharacterInputDriver::tick(Registry& registry, const InputActions& actions,
                                const glm::vec3& camera_forward, Entity& possessed, float dt) {
	const bool jump_pressed = actions.jump && !m_jump_was_down;
	m_jump_was_down = actions.jump;

	if (possessed != m_driven) {
		releaseCharacter(registry, m_driven, m_air_posed);
		m_driven = possessed;
		m_air_time = 0.0f;
		m_air_posed = false;
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

	// Rotation, two layers: yaw slerps toward the movement heading, then the
	// airborne lean pitches about the heading's right axis. The heading is
	// world-space but the component stores a local rotation.
	Entity parent = registry.getParent(possessed);
	glm::quat parent_rot = parent.isNull() ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
	                                       : registry.getWorldRotation(parent);
	glm::quat yaw_rot = twistZ(parent_rot * tc->getRotation());

	if (input_len > 1e-4f) {
		float target_yaw = std::atan2(-heading.x, heading.y) + glm::radians(cc->facing_offset_deg);
		glm::quat target_rot = glm::angleAxis(target_yaw, glm::vec3(0.0f, 0.0f, 1.0f));

		float ang = glm::angle(target_rot * glm::inverse(yaw_rot));
		if (ang > glm::pi<float>())
			ang = glm::two_pi<float>() - ang;
		if (ang > 1e-4f) {
			float t = std::min(1.0f, glm::radians(cc->turn_rate_deg) * dt / ang);
			yaw_rot = glm::normalize(glm::slerp(yaw_rot, target_rot, t));
		}
	}

	// Nose up ascending, down descending; full lean at |vz| = jump_speed
	float target_pitch = 0.0f;
	if (!cc->grounded && cc->jump_speed > 1e-4f) {
		float vz01 = glm::clamp(cc->velocity.z / cc->jump_speed, -1.0f, 1.0f);
		target_pitch = glm::radians(vz01 > 0.0f ? cc->air_pitch_up_deg : cc->air_pitch_down_deg) * vz01;
	}
	float max_step = AIR_PITCH_RATE * dt;
	m_air_pitch += glm::clamp(target_pitch - m_air_pitch, -max_step, max_step);

	bool pitched = std::abs(m_air_pitch) > 1e-4f;
	if (input_len > 1e-4f || pitched || m_was_pitched) {
		glm::quat world_rot = yaw_rot;
		if (pitched) {
			// The mesh front points along the heading the yaw aims at
			glm::vec3 front = yaw_rot * (glm::angleAxis(-glm::radians(cc->facing_offset_deg),
			                                            glm::vec3(0.0f, 0.0f, 1.0f)) * glm::vec3(0.0f, 1.0f, 0.0f));
			glm::vec3 lean_axis = glm::normalize(glm::cross(front, glm::vec3(0.0f, 0.0f, 1.0f)));
			world_rot = glm::angleAxis(m_air_pitch, lean_axis) * yaw_rot;
		}
		if (!parent.isNull())
			world_rot = glm::inverse(parent_rot) * world_rot;
		tc->setRotation(glm::normalize(world_rot));
	}
	m_was_pitched = pitched;

	if (jump_pressed)
		cc->jump_requested = true;

	if (auto* anim = registry.getComponent<AnimatorComponent>(possessed)) {
		// Airborne: hold the frozen air pose instead of the gait
		if (anim->getAirPoseClip() >= 0) {
			uint32_t air = static_cast<uint32_t>(anim->getAirPoseClip());
			m_air_time = cc->grounded ? 0.0f : m_air_time + dt;
			if (!m_air_posed && m_air_time > AIR_POSE_DELAY) {
				anim->crossFadeTo(air, AIR_POSE_FADE_IN);
				// crossFadeTo rewinds a stopped clip; re-pin the pose frame
				anim->setTime(air, anim->getAirPoseTime());
				m_air_posed = true;
			} else if (m_air_posed && cc->grounded) {
				anim->fadeClipWeight(air, 0.0f, AIR_POSE_FADE_OUT);
				m_air_posed = false; // setBlendParameter below resumes the gait
			}
		}

		if (!m_air_posed && anim->hasBlendSpace()) {
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