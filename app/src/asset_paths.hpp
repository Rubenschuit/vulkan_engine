#pragma once
#include <filesystem>

namespace ve {

// All asset paths in one place. Initialized from project_root in Sandbox constructor.
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
	std::filesystem::path particle_texture;
	std::filesystem::path fire_texture;
	std::filesystem::path smoke_texture;
	std::filesystem::path mots_texture;
	std::filesystem::path skybox_dir;
	// shaders
	std::filesystem::path shaders_dir;

	std::filesystem::path shader(const char* name) const { return shaders_dir / name; }
	std::filesystem::path sponza_model(const char* variant = "") const { return project_root / "models" / variant / "glTF" / "Sponza.gltf"; }
	std::filesystem::path bistro_model() const { return project_root / "models" / "bistro-master" / "bistro.gltf"; }
	std::filesystem::path mybistro_model() const { return project_root / "models" / "mybistro" / "bistro.gltf"; }

	explicit AssetPaths(const std::filesystem::path& root)
		: project_root(root),
		  cube_model(root / "models" / "cube.gltf"),
		  quad_model(root / "models" / "quad.gltf"),
		  sphere_model(root / "models" / "sphere" / "scene.gltf"),
		  flat_vase_model(root / "models" / "flat_vase.gltf"),
		  smooth_vase_model(root / "models" / "smooth_vase.gltf"),
		  viking_room_model(root / "models" / "viking_room.gltf"),
		  particle_texture(root / "textures" / "light.ktx2"),
		  fire_texture(root / "textures" / "fire_ball.ktx"),
		  smoke_texture(root / "textures" / "smoke_atlas.ktx2"),
		  mots_texture(root / "textures" / "mots.png"),
		  skybox_dir(root / "textures" / "skybox"),
		  shaders_dir(root / "shaders") {}
};

} // namespace ve
