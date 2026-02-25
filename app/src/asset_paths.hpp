#pragma once
#include <filesystem>

namespace ve {

// App-specific asset paths. Engine paths (shaders, skybox, cube, particle textures)
// are now in EngineConfig, passed to VeApplication's constructor.
struct AssetPaths {
	std::filesystem::path project_root;
	// models
	std::filesystem::path cube_model;
	std::filesystem::path quad_model;
	std::filesystem::path sphere_model;
	std::filesystem::path flat_vase_model;
	std::filesystem::path smooth_vase_model;
	std::filesystem::path viking_room_model;
	// textures
	std::filesystem::path mots_texture;
	std::filesystem::path grid_texture;

	std::filesystem::path sponza_model() const { return project_root / "models" / "sponza" / "glTF" / "Sponza.gltf"; }
	std::filesystem::path bistro_model() const { return project_root / "models" / "bistro-master" / "bistro.gltf"; }

	explicit AssetPaths(const std::filesystem::path& root)
		: project_root(root),
		  cube_model(root / "models" / "cube.gltf"),
		  quad_model(root / "models" / "quad.gltf"),
		  sphere_model(root / "models" / "sphere" / "sphere.gltf"),
		  flat_vase_model(root / "models" / "flat_vase.gltf"),
		  smooth_vase_model(root / "models" / "smooth_vase.gltf"),
		  viking_room_model(root / "models" / "viking_room.gltf"),
		  mots_texture(root / "textures" / "mots.png"),
		  grid_texture(root / "textures" / "grid.ktx") {}
};

} // namespace ve
