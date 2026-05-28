#pragma once
#include "ve_export.hpp"
#include <cstdint>
#include <filesystem>

namespace ve {

struct VENGINE_API EngineConfig {
	std::string app_name = "App";
	std::filesystem::path working_dir;
	std::filesystem::path shaders_dir;       // compiled .spv files
	std::filesystem::path skybox_dir;        // .ktx/.ktx2 skybox textures

	// optional
	std::filesystem::path light_billboard_texture;

	uint32_t default_particle_capacity = 200000;
	uint32_t max_particle_capacity = 2000000;

	// Window
	uint32_t window_width = 1920;
	uint32_t window_height = 1080;

	bool register_default_window_hotkeys = true;
};

}
