/* Intermediate data structures for async asset loading.
 * Produced by a background CPU thread (glTF parsing, image decoding, mesh processing),
 * consumed by the main thread for GPU resource creation.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_texture.hpp"
#include "resources/ve_material_properties.hpp"
#include "resources/ve_model.hpp"
#include "resources/ve_animation_clip.hpp"
#include "rendering/culling/meshlet_data.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

namespace ve {

struct DecodedTexture {
	std::string resource_id;
	std::filesystem::path file_path;
	TextureType type = TextureType::ALBEDO;
	bool is_default = false;			// true = use engine default texture, skip decode
};

struct ProcessedMesh {
	std::string resource_id;
	std::vector<VeMesh::Vertex> vertices;
	std::vector<VeMesh::SkinVertex> skin_vertices;  // empty if mesh has no JOINTS_0/WEIGHTS_0
	std::vector<VeMesh::AABB> joint_mesh_local_extents;  // per joint index, in mesh-local space
	std::vector<uint32_t> indices;
	std::vector<std::vector<uint32_t>> lod_indices;
	VeMesh::AABB local_aabb{};
	std::vector<glm::vec3> cpu_positions;
	std::vector<uint32_t> cpu_indices;
	std::unique_ptr<CpuMeshletData> meshlet_data;

	ProcessedMesh() = default;
	ProcessedMesh(ProcessedMesh&&) = default;
	ProcessedMesh& operator=(ProcessedMesh&&) = default;
	ProcessedMesh(const ProcessedMesh&) = delete;
	ProcessedMesh& operator=(const ProcessedMesh&) = delete;
};

// Texture indices refer into LoadedAssetData::textures (-1 = use default).
struct ProcessedMaterial {
	std::string resource_id;
	int albedo_tex_idx = -1;
	int normal_tex_idx = -1;
	int metallic_roughness_tex_idx = -1;
	int occlusion_tex_idx = -1;
	int emissive_tex_idx = -1;
	MaterialAlphaProps alpha_props;
	MaterialFactors factors;
	bool flip_tex_coord_v = false;
};

struct VENGINE_API LoadedAssetData {
	std::vector<DecodedTexture> textures;
	std::vector<ProcessedMesh> meshes;
	std::vector<ProcessedMaterial> materials;
	std::vector<ModelNode> nodes;
	std::vector<std::pair<uint32_t, uint32_t>> parent_links;	// (child_index, parent_index)
	std::unordered_set<uint32_t> root_indices;
	std::vector<VeModel::ExtractedLight> punctual_lights;
	std::vector<VeModel::ExtractedLight> emissive_lights;
	std::unordered_map<int, uint32_t> gltf_to_loaded_idx;
	std::vector<VeAnimationClip> animation_clips;
	std::vector<ModelSkin> skins;
	std::unordered_map<int, std::pair<glm::vec3, float>> geometry_center_extent;

	LoadedAssetData() = default;
	LoadedAssetData(LoadedAssetData&&) = default;
	LoadedAssetData& operator=(LoadedAssetData&&) = default;
	LoadedAssetData(const LoadedAssetData&) = delete;
	LoadedAssetData& operator=(const LoadedAssetData&) = delete;
};

// Thread-safe progress tracking for async loading.
// Each texture decode, texture upload, mesh process, mesh upload, and material
// creation counts as one item. Progress = completed_items / total_items.
struct VENGINE_API LoadProgress {
	std::atomic<uint32_t> completed_items{0};
	std::atomic<uint32_t> total_items{0};
	std::atomic<bool> cancelled{false};
	std::atomic<bool> cpu_done{false};
	std::atomic<bool> cpu_failed{false};
	std::mutex status_mutex;
	std::string status;

	void setStatus(const std::string& msg) {
		std::lock_guard lock(status_mutex);
		status = msg;
	}

	std::string getStatus() {
		std::lock_guard lock(status_mutex);
		return status;
	}

	float progress() const {
		uint32_t total = total_items.load();
		if (total == 0)
			return 0.f;
		return static_cast<float>(completed_items.load()) / static_cast<float>(total);
	}

	void reset() {
		completed_items = 0;
		total_items = 0;
		cancelled = false;
		cpu_done = false;
		cpu_failed = false;
		std::lock_guard lock(status_mutex);
		status.clear();
	}
};

} // namespace ve
