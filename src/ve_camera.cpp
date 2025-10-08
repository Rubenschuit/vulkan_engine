#include "ve_camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace ve {

	VeCamera::VeCamera(glm::vec3 position, glm::vec3 world_up)
		: pos(position), world_up(world_up) {
		lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
		updateView();
		updateProjection();
	}

	void VeCamera::setPosition(const glm::vec3& p) {
		pos = p;
		viewDirty = true;
	}

	void VeCamera::setYawPitch(float yaw_rad, float pitch_rad) {
		yaw = yaw_rad;
		pitch = pitch_rad;
		clampPitch();
		viewDirty = true;
	}

	void VeCamera::yawBy(float delta_rad) {
		yaw += delta_rad;
		wrapYaw();
		viewDirty = true;
	}

	void VeCamera::pitchBy(float delta_rad) {
		pitch += delta_rad;
		clampPitch();
		viewDirty = true;
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
	void VeCamera::lookAt(const glm::vec3& target) {
		glm::vec3 dir = target - pos;
		float len = glm::length(dir);
		if (len < 1e-6f) {
			return; // nothing to do
		}
		dir = glm::normalize(dir);

		pitch = std::asin(glm::dot(dir, world_up));

		// Yaw from XY projection; mapping matches forward = (-cos p cos y, cos p sin y, sin p)
		glm::vec2 xy{dir.x, dir.y};
		float xy_len = glm::length(xy);
		if (xy_len > 1e-6f) {
			glm::vec2 d = glm::normalize(xy);
			yaw = std::atan2(d.y, -d.x);
			wrapYaw();
		}
		clampPitch();
		viewDirty = true;
	}

	void VeCamera::moveForward(float d) {
		pos += forward * d;
		viewDirty = true;
	}

	void VeCamera::moveRight(float d) {
		pos += right * d;
		viewDirty = true;
	}

	void VeCamera::moveUpWorld(float d) {
		pos += world_up * d;
		viewDirty = true;
	}

	void VeCamera::setPerspective(float fov_y_radians, float aspect, float near_z, float far_z) {
		fovY = fov_y_radians;
		aspectRatio = aspect;
		zNear = near_z;
		zFar = far_z;
		updateProjection();
	}

	void VeCamera::updateIfDirty() {
		if (viewDirty) {
			updateView();
			viewDirty = false;
		}
	}

	void VeCamera::clampPitch() {
		const float lim = glm::radians(89.0f);
		if (pitch > lim) pitch = lim;
		else if (pitch < -lim) pitch = -lim;
	}

	void VeCamera::wrapYaw() {
		if (yaw > glm::pi<float>()) yaw -= glm::two_pi<float>();
		else if (yaw < -glm::pi<float>()) yaw += glm::two_pi<float>();
	}

	// Update the camera orthonormal basis vectors and then update
	// the view matrix
	// TODO: currently assuemes world_up is Z-up
	void VeCamera::updateView() {
		forward = glm::normalize(glm::vec3(
			-1.0f * std::cos(pitch) * std::cos(yaw),
			std::cos(pitch) * std::sin(yaw),
			std::sin(pitch)

		));
		right = glm::normalize(glm::cross(forward, world_up));
		up = glm::normalize(glm::cross(right, forward));
		view = glm::lookAt(pos, pos + forward, up);
	}

	void VeCamera::updateProjection() {
		proj = glm::perspective(fovY, aspectRatio, zNear, zFar);
		proj[1][1] *= -1.0f; // Flip Y for non OpenGL coordinate system
	}

} // namespace ve
