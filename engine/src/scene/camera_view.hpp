#pragma once
#include "ve_entity.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <optional>

namespace ve {

class Registry;

// Per-frame camera snapshot consumed by every render system through VeFrameInfo.
// Built once per frame by VeApplication from either the editor camera or a scene
// entity with a CameraComponent.
//
// proj uses infinite reverse-Z with a Vulkan Y-flip: proj[1][1] = -1/tan(fov/2),
// proj[2][2] = 0, proj[3][2] = z_near.
struct CameraView {
	glm::mat4 view{1.0f};
	glm::mat4 proj{1.0f};
	glm::mat4 inv_view{1.0f};
	glm::vec3 position{0.0f};
	glm::vec3 forward{-1.0f, 0.0f, 0.0f};
	glm::vec3 right{0.0f, -1.0f, 0.0f};
	glm::vec3 up{0.0f, 0.0f, 1.0f};
	float fov_y_radians = glm::radians(55.0f);
	float aspect = 4.0f / 3.0f;
	float z_near = 0.1f;
	float z_far = 1000.0f;
	Entity source = Entity::null();
};

CameraView buildCameraView(const Registry& registry, Entity entity, float aspect);

// True when `camera` is alive in `registry` and carries a CameraComponent
bool isSceneCamera(const Registry* registry, Entity camera);

// Basis of an entity camera: columns are {right, forward, up}. Identity rotation
// looks +Y, right +X, up +Z. Scale is stripped.
glm::mat3 sceneCameraBasis(const Registry& registry, Entity entity);

// Returns a CameraView built from `camera` if isSceneCamera(registry, camera);
// otherwise nullopt
std::optional<CameraView> tryGetSceneCamera(const Registry* registry, Entity camera, float aspect);

}