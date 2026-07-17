#pragma once
#include "VEngine/VEngine.hpp"

#include <filesystem>

namespace ve {

// Survey/Walk/Run blend space, airborne pose,
// character controller, and a follow camera. Hand the result to VeScene::placeModel.
AddModelRequestedEvent foxModelRequest(Registry& registry, const std::filesystem::path& gltf_path,
                                       glm::vec3 translation);

}