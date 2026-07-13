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
	std::filesystem::path birds_model;
	std::filesystem::path pigeon_model;
	std::filesystem::path fox_model;
	// textures
	std::filesystem::path mots_texture;
	std::filesystem::path grid_texture;
	std::filesystem::path fire_texture;
	std::filesystem::path smoke_texture;

	std::filesystem::path sponza_model() const { return project_root / "models" / "sponza" / "glTF" / "Sponza.gltf"; }
	std::filesystem::path bistro_model() const { return project_root / "models" / "bistro-master" / "bistro.gltf"; }

	explicit AssetPaths(const std::filesystem::path& root)
		: project_root(root),
		  cube_model(root / "models" / "cube.gltf"),
		  quad_model(root / "models" / "quad.gltf"),
		  sphere_model(root / "models" / "sphere" / "sphere.gltf"),
		  flat_vase_model(root / "models" / "flat_vase.gltf"),
		  smooth_vase_model(root / "models" / "smooth_vase.gltf"),
		  birds_model(root / "models" / "birds.glb"),
		  pigeon_model(root / "models" / "pigeon.glb"),
		  fox_model(root / "models" / "Fox" / "glTF" / "Fox.gltf"),
		  mots_texture(root / "textures" / "mots.png"),
		  grid_texture(root / "textures" / "grid.ktx"),
		  fire_texture(root / "textures" / "fire_ball.ktx"),
		  smoke_texture(root / "textures" / "smoke_atlas.ktx2") {}
};

}
