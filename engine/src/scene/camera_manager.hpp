/* CameraManager - owns the fly camera, advances the active camera each frame, and
 * resolves which one renders. Selection is one explicit entity (null = fly).
 */
#pragma once
#include "ve_export.hpp"
#include "scene/camera_sweep.hpp"
#include "scene/camera_view.hpp"
#include "scene/fly_camera_controller.hpp"

namespace ve {

class Registry;
struct InputActions;

class VENGINE_API CameraManager {
public:
	void setSweepFn(CameraSweepFn fn) { m_sweep = std::move(fn); }

	// Forward of the camera that will render (entity cameras: +Y), for
	// camera-relative input. Reads last frame's pose; advances nothing.
	glm::vec3 activeForward(const Registry* registry, Entity active) const;

	// Advance the active camera: a follow rig ticks its orbit (call after physics,
	// so the pivot is the post-step position), anything else ticks the fly camera.
	// False when a follow camera's target died, so the caller can clear the selection.
	bool tick(Registry* registry, Entity active, const InputActions& actions, float dt);

	// Entity camera if valid, fly camera otherwise
	const CameraView& resolveView(const Registry* registry, Entity active,
	                              float aspect, float fov_y_radians);

	const CameraView& currentView() const { return m_current_view; }

	FlyCameraController& flyCamera() { return m_fly_camera; }
	const FlyCameraController& flyCamera() const { return m_fly_camera; }

private:
	FlyCameraController m_fly_camera;
	CameraView m_current_view{};
	float m_last_aspect = 16.0f / 9.0f;
	CameraSweepFn m_sweep;
};

}