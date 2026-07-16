/* CameraManager - owns the free-fly camera and resolves which camera renders
 * each frame. Editor-independent.
 */
#pragma once
#include "ve_export.hpp"
#include "scene/camera_view.hpp"
#include "scene/fly_camera_controller.hpp"

namespace ve {

class Registry;

class VENGINE_API CameraManager {
public:
	// Explicit override entity (alive + CameraComponent) wins; fly camera otherwise.
	const CameraView& resolveView(const Registry* registry, Entity override_camera,
	                              float aspect, float fov_y_radians);

	const CameraView& currentView() const { return m_current_view; }

	FlyCameraController& flyCamera() { return m_fly_camera; }
	const FlyCameraController& flyCamera() const { return m_fly_camera; }

private:
	FlyCameraController m_fly_camera;
	CameraView m_current_view{};
	float m_last_aspect = 16.0f / 9.0f;
};

} 