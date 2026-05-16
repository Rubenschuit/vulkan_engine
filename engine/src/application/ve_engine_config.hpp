#pragma once
#include "ve_export.hpp"
#include <filesystem>

namespace ve {

struct VENGINE_API EngineConfig {
	std::string app_name = "App";            // used for window titles (e.g. "App Settings")
	std::filesystem::path working_dir;       // project root
	std::filesystem::path shaders_dir;       // compiled .spv files
	std::filesystem::path skybox_dir;        // .ktx/.ktx2 skybox textures

	struct ParticleAssets {
		std::filesystem::path glow;
		std::filesystem::path fire;
		std::filesystem::path smoke;
	} particle_assets;
};

} // namespace ve