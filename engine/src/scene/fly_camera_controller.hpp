#pragma once
#include "ve_export.hpp"
#include "scene/camera_view.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace ve {

struct InputActions;

class VENGINE_API FlyCameraController {
public:
	void tick(const InputActions& actions, float dt);
	CameraView buildView(float aspect) const;

	void setPosition(glm::vec3 p) { m_position = p; }
	void setYawPitch(float yaw_rad, float pitch_rad);
	void lookAt(const glm::vec3& target);

	glm::vec3 position() const { return m_position; }
	glm::vec3 forward() const;
	float yaw() const { return m_yaw_rad; }
	float pitch() const { return m_pitch_rad; }

	void setFov(float fov_y_radians) { m_fov_y_radians = fov_y_radians; }
	float fov() const { return m_fov_y_radians; }
	void setNearFar(float near_z, float far_z) { m_near = near_z; m_far = far_z; }

	static constexpr float DEFAULT_MOVE_SPEED = 5.0f;
	static constexpr float DEFAULT_SPRINT_MULTIPLIER = 12.0f;
	static constexpr float DEFAULT_MOUSE_SENSITIVITY = 0.2f; // degrees per pixel

	float moveSpeed() const { return m_move_speed; }
	void setMoveSpeed(float metres_per_second) { m_move_speed = metres_per_second; }
	float sprintMultiplier() const { return m_sprint_multiplier; }
	void setSprintMultiplier(float multiplier) { m_sprint_multiplier = multiplier; }
	float mouseSensitivity() const { return m_mouse_sensitivity; }
	void setMouseSensitivity(float degrees_per_pixel) { m_mouse_sensitivity = degrees_per_pixel; }

private:
	glm::vec3 m_position{20.0f, 20.0f, 20.0f};
	float m_yaw_rad = 0.0f;
	float m_pitch_rad = 0.0f;
	float m_fov_y_radians = glm::radians(80.0f);
	float m_near = 0.1f;
	float m_far = 100000.0f;

	float m_move_speed = DEFAULT_MOVE_SPEED;
	float m_sprint_multiplier = DEFAULT_SPRINT_MULTIPLIER;
	float m_look_speed = 2.0f; // arrow keys
	float m_mouse_sensitivity = DEFAULT_MOUSE_SENSITIVITY;
};

} // namespace ve