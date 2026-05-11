#pragma once
#include "ve_export.hpp"
#include "scene/camera_view.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace ve {

struct InputActions;

class VENGINE_API EditorCameraController {
public:
	void tick(const InputActions& actions, float dt);
	CameraView buildView(float aspect) const;

	void setPosition(glm::vec3 p) { m_position = p; }
	void setYawPitch(float yaw_rad, float pitch_rad);
	void lookAt(const glm::vec3& target);

	glm::vec3 position() const { return m_position; }
	float yaw() const { return m_yaw_rad; }
	float pitch() const { return m_pitch_rad; }

	void setFov(float fov_y_radians) { m_fov_y_radians = fov_y_radians; }
	float fov() const { return m_fov_y_radians; }
	void setNearFar(float near_z, float far_z) { m_near = near_z; m_far = far_z; }

private:
	glm::vec3 m_position{20.0f, 20.0f, 20.0f};
	float m_yaw_rad = 0.0f;
	float m_pitch_rad = 0.0f;
	float m_fov_y_radians = glm::radians(80.0f);
	float m_near = 0.1f;
	float m_far = 100000.0f;

	float m_normal_speed = 5.0f;
	float m_sprint_speed = 60.0f;
	float m_look_speed = 2.0f; // arrow keys
	float m_mouse_sensitivity = 0.2f;
};

} // namespace ve