#pragma once
#include "ve_export.hpp"
#include <filesystem>

namespace ve {

struct VENGINE_API EngineConfig {
	std::string app_name = "App";            // used for window titles (e.g. "App Settings")
	std::filesystem::path working_dir;       // project root
	std::filesystem::path shaders_dir;       // compiled .spv files
	std::filesystem::path skybox_dir;        // .ktx/.ktx2 skybox textures
	std::filesystem::path cube_model;        // cube.gltf for skybox mesh
	std::filesystem::path particle_texture;
	std::filesystem::path fire_texture;
	std::filesystem::path smoke_texture;
};

} // namespace ve