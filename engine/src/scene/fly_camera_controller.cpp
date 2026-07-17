#include "pch.hpp"
#include "scene/fly_camera_controller.hpp"
#include "scene/camera_math.hpp"
#include "input/input_action.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace ve {

void FlyCameraController::setYawPitch(float yaw_rad, float pitch_rad) {
	m_yaw_rad = yaw_rad;
	m_pitch_rad = pitch_rad;
	wrapYaw(m_yaw_rad);
	clampPitch(m_pitch_rad);
}

void FlyCameraController::lookAt(const glm::vec3& target) {
	glm::vec3 dir = target - m_position;
	if (glm::length(dir) < 1e-6f)
		return;
	glm::vec2 yp = yawPitchFromForward(dir, m_yaw_rad);
	m_yaw_rad = yp.x;
	m_pitch_rad = yp.y;
	clampPitch(m_pitch_rad);
}

glm::vec3 FlyCameraController::forward() const {
	return forwardFromYawPitch(m_yaw_rad, m_pitch_rad);
}

void FlyCameraController::tick(const InputActions& actions, float dt) {
	float yaw_delta   = actions.look_yaw   * m_look_speed * dt;
	float pitch_delta = actions.look_pitch * m_look_speed * dt;
	if (actions.mouse_look_enabled) {
		yaw_delta   += glm::radians(actions.mouse_dx * m_mouse_sensitivity);
		pitch_delta += glm::radians(actions.mouse_dy * m_mouse_sensitivity);
	}
	m_yaw_rad   += yaw_delta;
	m_pitch_rad += pitch_delta;
	wrapYaw(m_yaw_rad);
	clampPitch(m_pitch_rad);

	glm::vec3 fwd   = forwardFromYawPitch(m_yaw_rad, m_pitch_rad);
	glm::vec3 right = glm::normalize(glm::cross(fwd, WORLD_UP));
	float speed = (actions.sprint ? m_sprint_speed : m_normal_speed) * dt;

	m_position += fwd      * (actions.move_forward * speed);
	m_position += right    * (actions.move_right   * speed);
	m_position += WORLD_UP * (actions.move_up      * speed);
}

CameraView FlyCameraController::buildView(float aspect) const {
	CameraView v;
	v.position = m_position;
	v.forward  = forwardFromYawPitch(m_yaw_rad, m_pitch_rad);
	v.right    = glm::normalize(glm::cross(v.forward, WORLD_UP));
	v.up       = glm::normalize(glm::cross(v.right, v.forward));
	v.view     = glm::lookAt(v.position, v.position + v.forward, v.up);
	v.inv_view = glm::inverse(v.view);

	// Infinite reverse-Z, Vulkan Y-flipped.
	float f = 1.0f / std::tan(m_fov_y_radians * 0.5f);
	v.proj = glm::mat4(0.0f);
	v.proj[0][0] = f / aspect;
	v.proj[1][1] = -f;
	v.proj[2][3] = -1.0f;
	v.proj[3][2] = m_near;

	v.fov_y_radians = m_fov_y_radians;
	v.aspect = aspect;
	v.z_near = m_near;
	v.z_far  = m_far;
	return v;
}

}