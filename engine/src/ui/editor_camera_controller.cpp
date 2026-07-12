#include "pch.hpp"
#include "ui/editor_camera_controller.hpp"
#include "input/input_action.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace ve {

static constexpr glm::vec3 WORLD_UP{0.0f, 0.0f, 1.0f};

static glm::vec3 forwardFromYawPitch(float yaw, float pitch) {
	return glm::normalize(glm::vec3(
		-std::cos(pitch) * std::cos(yaw),
		 std::cos(pitch) * std::sin(yaw),
		 std::sin(pitch)));
}

static void clampPitch(float& p) {
	constexpr float lim = glm::radians(89.0f);
	if (p > lim)
		p = lim;
	else if (p < -lim)
		p = -lim;
}

static void wrapYaw(float& y) {
	if (y > glm::pi<float>())
		y -= glm::two_pi<float>();
	else if (y < -glm::pi<float>())
		y += glm::two_pi<float>();
}

void EditorCameraController::setYawPitch(float yaw_rad, float pitch_rad) {
	m_yaw_rad = yaw_rad;
	m_pitch_rad = pitch_rad;
	wrapYaw(m_yaw_rad);
	clampPitch(m_pitch_rad);
}

// Compute yaw/pitch consistent with forwardFromYawPitch. We have
//
//    -cos(pitch) * cos(yaw)        dir.x
// 	  cos(pitch) * sin(yaw)    =    dir.y
//	    	  sin(pitch)           	dir.z ,
//
// therefore
//
//	   pitch = arcsin(dir.z).
//
// The yaw angle follows from
//
//     (dir.y / -dir.x) = sin(yaw) / cos(yaw) = tan(yaw),
//
// so
//
//     yaw = arctan( dir.y / -dir.x).
//
// Somewhat expensive, but only called when the camera is re-oriented
// to look at a specific point.
void EditorCameraController::lookAt(const glm::vec3& target) {
	glm::vec3 dir = target - m_position;
	float len = glm::length(dir);
	if (len < 1e-6f)
		return;
	dir = glm::normalize(dir);

	m_pitch_rad = std::asin(glm::dot(dir, WORLD_UP));

	glm::vec2 xy{dir.x, dir.y};
	float xy_len = glm::length(xy);
	if (xy_len > 1e-6f) {
		glm::vec2 d = glm::normalize(xy);
		m_yaw_rad = std::atan2(d.y, -d.x);
		wrapYaw(m_yaw_rad);
	}
	clampPitch(m_pitch_rad);
}

glm::vec3 EditorCameraController::forward() const {
	return forwardFromYawPitch(m_yaw_rad, m_pitch_rad);
}

void EditorCameraController::tick(const InputActions& actions, float dt) {
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

CameraView EditorCameraController::buildView(float aspect) const {
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

} // namespace ve