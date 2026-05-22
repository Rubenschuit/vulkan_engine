/* Loads gltf or glb files.
 * Parses, decodes textures, processes meshes (MikkTSpace, meshopt,
 * LOD, meshlet), extracts lights/cameras/animations/skins. Vulkan-free and
 * thread-safe; produces a LoadedAssetData that the main thread can upload via
 * uploadLoadedAssetStep.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/internal/loaded_asset_data.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"

#include <filesystem>

namespace ve::gltf {

// Parse a glTF/glb file and produce engine-ready intermediate data.
// extract_lights: parse KHR_lights_punctual + emissive-as-lights
LoadedAssetData load(const std::filesystem::path& model_path,
                     bool extract_lights, bool flip_tex_coord_v,
                     LoadProgress& progress,
                     GpuCaps gpu_caps);

// Process only the first primitive that has a valid position
// accessor, upload it, and return a mesh handle. Skips materials, textures,
// animations, lights, and the rest of the scene graph. Returns an invalid
// handle on failure.
ResourceHandle<VeMesh> loadFirstMesh(VeResourceManager& rm,
                                     const std::filesystem::path& model_path);

}