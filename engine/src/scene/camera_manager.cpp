#include "pch.hpp"
#include "scene/camera_manager.hpp"
#include "scene/camera_math.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"

namespace ve {

glm::vec3 CameraManager::activeForward(const Registry* registry, Entity active) const {
	if (isSceneCamera(registry, active))
		return sceneCameraBasis(*registry, active)[1];
	return m_fly_camera.forward();
}

bool CameraManager::tick(Registry* registry, Entity active, const InputActions& actions, float dt) {
	if (isSceneCamera(registry, active)) {
		if (auto* fc = registry->getComponent<FollowCameraComponent>(active))
			return fc->tick(m_sweep, actions, dt);
		return true;
	}
	m_fly_camera.tick(actions, dt);
	return true;
}

const CameraView& CameraManager::resolveView(const Registry* registry, Entity active,
                                             float aspect, float fov_y_radians) {
	if (aspect > 0.0f)
		m_last_aspect = aspect;

	m_fly_camera.setFov(fov_y_radians);

	if (auto scene_cam = tryGetSceneCamera(registry, active, m_last_aspect)) {
		m_current_view = *scene_cam;
		return m_current_view;
	}

	// Exiting an entity camera: seed the fly camera from the last rendered view 
	if (!m_current_view.source.isNull()) {
		m_fly_camera.setPosition(m_current_view.position);
		glm::vec2 yp = yawPitchFromForward(m_current_view.forward, m_fly_camera.yaw());
		m_fly_camera.setYawPitch(yp.x, yp.y);
	}

	m_current_view = m_fly_camera.buildView(m_last_aspect);
	return m_current_view;
}

}