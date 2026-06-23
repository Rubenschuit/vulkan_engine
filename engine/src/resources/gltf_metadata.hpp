#pragma once
#include "ve_export.hpp"
#include <cstdint>
#include <filesystem>
#include <string>

namespace ve::gltf {

struct GltfMetadata {
	uint32_t mesh_count = 0;
	uint32_t material_count = 0;
	uint32_t node_count = 0;
	uint32_t animation_count = 0;
	uint32_t skin_count = 0;
	uint32_t light_count = 0;
	uint32_t camera_count = 0;
	uint32_t texture_count = 0;
	uint64_t triangle_count = 0;
	bool ok = false;
	std::string error;
};

// Parse only the header of a glTF/glb and report asset stats
VENGINE_API GltfMetadata probeMetadata(const std::filesystem::path& model_path);

}