#include "ve_camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace ve {

	VeCamera::VeCamera(glm::vec3 position, glm::vec3 world_up)
		: m_pos(position), m_world_up(world_up) {
		lookAt(glm::vec3(0.0f, 0.0f, 5.0f));
		updateView();
		updateProjection();
	}

	void VeCamera::setPosition(const glm::vec3& p) {
		m_pos = p;
		m_view_dirty = true;
	}

	void VeCamera::setYawPitch(float yaw_rad, float pitch_rad) {
		m_yaw = yaw_rad;
		m_pitch = pitch_rad;
		clampPitch();
		m_view_dirty = true;
	}

	void VeCamera::yawBy(float delta_rad) {
		m_yaw += delta_rad;
		wrapYaw();
		m_view_dirty = true;
	}

	void VeCamera::pitchBy(float delta_rad) {
		m_pitch += delta_rad;
		clampPitch();
		m_view_dirty = true;
	}

	// Compute yaw/pitch consistent with updateView. We have
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
	void VeCamera::lookAt(const glm::vec3& target) {
		glm::vec3 dir = target - m_pos;
		float len = glm::length(dir);
		if (len < 1e-6f) {
			return; // nothing to do
		}
		dir = glm::normalize(dir);

		m_pitch = std::asin(glm::dot(dir, m_world_up));

		// Yaw from XY projection
		glm::vec2 xy{dir.x, dir.y};
		float xy_len = glm::length(xy);
		if (xy_len > 1e-6f) {
			glm::vec2 d = glm::normalize(xy);
			m_yaw = std::atan2(d.y, -d.x);
			wrapYaw();
		}
		clampPitch();
		m_view_dirty = true;
	}

	void VeCamera::moveForward(float d) {
		m_pos += m_forward * d;
		m_view_dirty = true;
	}

	void VeCamera::moveRight(float d) {
		m_pos += m_right * d;
		m_view_dirty = true;
	}

	void VeCamera::moveUpWorld(float d) {
		m_pos += m_world_up * d;
		m_view_dirty = true;
	}

	void VeCamera::setPerspective(float fov_y_radians, float aspect, float near_z, float far_z) {
		m_fov_y = fov_y_radians;
		m_aspect_ratio = aspect;
		m_z_near = near_z;
		m_z_far = far_z;
		updateProjection();
	}

	void VeCamera::updateIfDirty() {
		if (m_view_dirty) {
			updateView();
			m_view_dirty = false;
		}
	}

	void VeCamera::clampPitch() {
		const float lim = glm::radians(89.0f);
		if (m_pitch > lim) m_pitch = lim;
		else if (m_pitch < -lim) m_pitch = -lim;
	}

	void VeCamera::wrapYaw() {
		if (m_yaw > glm::pi<float>()) m_yaw -= glm::two_pi<float>();
		else if (m_yaw < -glm::pi<float>()) m_yaw += glm::two_pi<float>();
	}

	// Update the camera orthonormal basis vectors and then update
	// the view matrix
	// TODO: currently assuemes world_up is Z-up
	void VeCamera::updateView() {
		m_forward = glm::normalize(glm::vec3(
			-1.0f * std::cos(m_pitch) * std::cos(m_yaw),
			std::cos(m_pitch) * std::sin(m_yaw),
			std::sin(m_pitch)

		));
		m_right = glm::normalize(glm::cross(m_forward, m_world_up));
		m_up = glm::normalize(glm::cross(m_right, m_forward));
		m_view = glm::lookAt(m_pos, m_pos + m_forward, m_up);
	}

	void VeCamera::updateProjection() {
		m_proj = glm::perspective(m_fov_y, m_aspect_ratio, m_z_near, m_z_far);
		m_proj[1][1] *= -1.0f; // Flip Y for non OpenGL coordinate system
	}

} // namespace ve
