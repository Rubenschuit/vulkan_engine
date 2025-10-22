/* Class representing a camera in 3D space, it controls the view and projection matrices
Only works for world up = +Z currently
improvements: - callbacks
				- quaternion-based rotation
				- pass events to event system
*/
#pragma once
#include "ve_export.hpp"
#include <glm/glm.hpp>

namespace ve {

class VENGINE_API VeCamera {
public:
	// World up defaults to +Z, position defaults to (2,2,2)
	// Looking down -X axis, yaw=0, pitch=0
	// FOV 55 degrees, aspect 4:3, near 0.1, far 100
	explicit VeCamera(glm::vec3 position = {10.0f, 10.0f, 10.0f}, glm::vec3 world_up = {0.0f, 0.0f, 1.0f});

	void setPosition(const glm::vec3& p);

	const glm::vec3& getPosition() const { return m_pos; }

	void setYawPitch(float yaw_rad, float pitch_rad);
	void yawBy(float delta_rad);
	void pitchBy(float delta_rad);

	// Point the camera at a target position in world space
	void lookAt(const glm::vec3& target);

	// Movement in camera basis/world up
	void moveForward(float d);
	void moveRight(float d);
	void moveUpWorld(float d);

	// Set aspect/FOV/clip
	void setPerspective(float fov_y_radians, float aspect, float near_z, float far_z);

	void updateIfDirty();

	const glm::mat4& getView() const { return m_view; }
	const glm::mat4& getProj() const { return m_proj; }
	const glm::vec3& getForward() const { return m_forward; }
	const glm::vec3& getRight() const { return m_right; }
	const glm::vec3& getUp() const { return m_up; }

private:
	void clampPitch();
	void wrapYaw();
	void updateView();
	void updateProjection();

	glm::vec3 m_pos;
	glm::vec3 m_world_up;
	float m_yaw{0.0f};   // radians
	float m_pitch{0.0f}; // radians
	bool m_view_dirty{true};

	// Derived
	glm::vec3 m_forward{1,0,0};
	glm::vec3 m_right{0,0,1};
	glm::vec3 m_up{0,1,0};
	glm::mat4 m_view{1.0f};

	// Projection
	float m_fov_y{glm::radians(55.0f)};
	float m_aspect_ratio{4.0f/3.0f};
	float m_z_near{0.1f};
	float m_z_far{100.0f};
	glm::mat4 m_proj{1.0f};
};
}
