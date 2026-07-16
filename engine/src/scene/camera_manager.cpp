#include "pch.hpp"
#include "scene/camera_manager.hpp"
#include "scene/ve_registry.hpp"

namespace ve {

const CameraView& CameraManager::resolveView(const Registry* registry, Entity override_camera,
                                             float aspect, float fov_y_radians) {
	if (aspect > 0.0f)
		m_last_aspect = aspect;

	m_fly_camera.setFov(fov_y_radians);

	auto scene_cam = tryGetSceneCamera(registry, override_camera, m_last_aspect);
	m_current_view = scene_cam ? *scene_cam : m_fly_camera.buildView(m_last_aspect);
	return m_current_view;
}

} 