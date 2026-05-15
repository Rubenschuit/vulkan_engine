#pragma once
#include "ve_export.hpp"
#include <optional>

namespace ve {

struct VENGINE_API ParticleSceneConfig {
	// Future: per-scene initial mode, capacity, origin, etc.
};

struct VENGINE_API FireworksSceneConfig {
	// Future: per-scene defaults for launch position, gravity, etc.
};

// Declared by a VeScene to opt into engine content subsystems.
struct VENGINE_API SceneSubsystems {
	std::optional<ParticleSceneConfig> particles;
	std::optional<FireworksSceneConfig> fireworks;
};

}