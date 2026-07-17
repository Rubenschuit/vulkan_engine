/* Camera occlusion probe: sweeps a sphere against static world geometry and returns
 * how far the sphere centre travelled before contact.
 * nullopt = clear path; a sweep starting inside geometry must report nullopt.
 */
#pragma once
#include <glm/glm.hpp>
#include <functional>
#include <optional>

namespace ve {

using CameraSweepFn = std::function<std::optional<float>(
	const glm::vec3& from, const glm::vec3& to, float radius)>;

}
