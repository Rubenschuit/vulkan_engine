#include "pch.hpp"
#include "scene/camera_view.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace ve {

glm::mat3 sceneCameraBasis(const Registry& registry, Entity entity) {
	return glm::mat3_cast(registry.getWorldRotation(entity));
}

bool isSceneCamera(const Registry* registry, Entity camera) {
	return registry && !camera.isNull() && registry->isAlive(camera)
	       && registry->hasComponent<CameraComponent>(camera);
}

CameraView buildCameraView(const Registry& registry, Entity entity, float aspect) {
	CameraView v;
	v.aspect = aspect;
	v.source = entity;

	const glm::mat4& world = registry.getWorldTransform(entity);

	// Identity rotation: forward = +Y, right = +X, up = +Z. Entity rotation rotates this basis.
	glm::mat3 R = sceneCameraBasis(registry, entity);
	v.right   = R[0];
	v.forward = R[1];
	v.up      = R[2];
	v.position = glm::vec3(world[3]);

	v.view = glm::lookAt(v.position, v.position + v.forward, v.up);
	v.inv_view = glm::inverse(v.view);

	const auto* cc = registry.getComponent<CameraComponent>(entity);
	if (cc) {
		v.fov_y_radians = cc->getFovY();
		v.z_near = cc->getNear();
		v.z_far  = cc->getFar();

		if (cc->getProjection() == CameraComponent::ProjectionType::Perspective) {
			// Infinite reverse-Z, Vulkan Y-flipped.
			float f = 1.0f / std::tan(cc->getFovY() * 0.5f);
			v.proj = glm::mat4(0.0f);
			v.proj[0][0] = f / aspect;
			v.proj[1][1] = -f;
			v.proj[2][3] = -1.0f;
			v.proj[3][2] = cc->getNear();
		} else {
			// Orthographic, reverse-Z (near -> z=1, far -> z=0), Vulkan Y-flipped.
			// ndc_z = (z + far)/(far - near).
			float h = cc->getOrthoSize();
			float w = h * aspect;
			float z_range = cc->getFar() - cc->getNear();
			v.proj = glm::mat4(0.0f);
			v.proj[0][0] =  1.0f / w;
			v.proj[1][1] = -1.0f / h;
			v.proj[2][2] =  1.0f / z_range;
			v.proj[3][2] = cc->getFar() / z_range;
			v.proj[3][3] = 1.0f;
		}
	}

	return v;
}

std::optional<CameraView> tryGetSceneCamera(const Registry* registry, Entity camera, float aspect) {
	if (!isSceneCamera(registry, camera))
		return std::nullopt;
	return buildCameraView(*registry, camera, aspect);
}

} // namespace ve
