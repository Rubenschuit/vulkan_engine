/* Yaw/pitch conventions shared by every engine camera.
 *
 * Z-up. yaw = 0 looks down -X and increases toward +Y; pitch = 0 is level,
 * positive looks up. 
 */
#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace ve {

inline constexpr glm::vec3 WORLD_UP{0.0f, 0.0f, 1.0f};
inline constexpr float DEFAULT_PITCH_LIMIT_DEG = 89.0f;

// Hard bound for cameras whose eye position is derived from their forward. At +-90
// cross(forward, WORLD_UP) is zero
inline constexpr float ORBIT_PITCH_LIMIT_DEG = 85.0f;
static_assert(ORBIT_PITCH_LIMIT_DEG < 90.0f);

inline glm::vec3 forwardFromYawPitch(float yaw, float pitch) {
	return glm::normalize(glm::vec3(
		-std::cos(pitch) * std::cos(yaw),
		 std::cos(pitch) * std::sin(yaw),
		 std::sin(pitch)));
}

inline void wrapYaw(float& y) {
	if (y > glm::pi<float>())
		y -= glm::two_pi<float>();
	else if (y < -glm::pi<float>())
		y += glm::two_pi<float>();
}

inline void clampPitch(float& p, float lim = glm::radians(DEFAULT_PITCH_LIMIT_DEG)) {
	if (p > lim)
		p = lim;
	else if (p < -lim)
		p = -lim;
}

// Asymmetric limits, for cameras whose eye position depends on the basis: an orbit
// camera must stay away from +-90 deg, where cross(forward, WORLD_UP) degenerates.
inline void clampPitchRange(float& p, float min_rad, float max_rad) {
	p = glm::clamp(p, min_rad, max_rad);
}

// Inverse of forwardFromYawPitch. `dir` need not be normalized; a near-vertical dir
// keeps yaw at `fallback_yaw`. Returns {yaw, pitch}.
//
// We have
//
//    -cos(pitch) * cos(yaw)        dir.x
// 	  cos(pitch) * sin(yaw)    =    dir.y
//	    	  sin(pitch)           	dir.z ,
//
// therefore
//
//	   pitch = arcsin(dir.z).
//
// The yaw angle follows from
//
//     (dir.y / -dir.x) = sin(yaw) / cos(yaw) = tan(yaw),
//
// so
//
//     yaw = arctan( dir.y / -dir.x).
//
// Somewhat expensive, but only called when a camera is re-oriented to look at a
// specific point.
inline glm::vec2 yawPitchFromForward(const glm::vec3& dir, float fallback_yaw = 0.0f) {
	float len = glm::length(dir);
	if (len < 1e-6f)
		return {fallback_yaw, 0.0f};
	glm::vec3 d = dir / len;

	float yaw = fallback_yaw;
	glm::vec2 xy{d.x, d.y};
	if (glm::length(xy) > 1e-6f) {
		glm::vec2 n = glm::normalize(xy);
		yaw = std::atan2(n.y, -n.x);
		wrapYaw(yaw);
	}
	float pitch = std::asin(glm::clamp(d.z, -1.0f, 1.0f));
	return {yaw, pitch};
}

} // namespace ve