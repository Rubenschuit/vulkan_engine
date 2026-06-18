/* Intermediate data structures for async asset loading.
 * Produced by a background CPU thread (glTF parsing, image decoding, mesh processing),
 * consumed by the main thread for GPU resource creation.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_texture.hpp"
#include "resources/ve_material_properties.hpp"
#include "resources/model_data.hpp"
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

#include <vulkan/vulkan.hpp>

namespace ve {

struct GpuCaps {
	bool supports_bc = false;
	bool supports_astc = false;
};

struct DecodedTexture {
	std::string resource_id;
	std::filesystem::path file_path;
	TextureType type = TextureType::ALBEDO;

	// Pre-decoded payload populated on the loader worker thread.
	// Empty pixels vector means the decode failed; the upload stage skips the slot
	// and createMaterial substitutes the engine default by type.
	std::vector<uint8_t> pixels;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mip_levels = 1;
	uint32_t array_layers = 1;
	bool is_cubemap = false;
	vk::Format format = vk::Format::eUndefined;

	// Per-mip byte offsets/extents into `pixels`. Empty => single-mip layout.
	std::vector<vk::DeviceSize> mip_offsets;
	std::vector<vk::Extent3D> mip_extents;
};

struct ProcessedMesh {
	std::string resource_id;
	std::vector<VeMesh::Vertex> vertices;
	// Position-only mirror of vertices[].pos. Precomputed on the worker so the
	// main-thread shadow-buffer upload is a memcpy, and reused as m_cpu_positions.
	std::vector<glm::vec3> shadow_positions;
	std::vector<VeMesh::SkinVertex> skin_vertices;  // empty if mesh has no JOINTS_0/WEIGHTS_0
	std::vector<VeMesh::AABB> joint_mesh_local_extents;  // per joint index, in mesh-local space

	uint32_t morph_target_count = 0;
	std::vector<glm::vec3> morph_pos_deltas;
	std::vector<glm::vec3> morph_normal_deltas;
	VeMesh::AABB morph_local_aabb{};  // conservative deformed bound
	std::vector<uint32_t> indices;
	std::vector<std::vector<uint32_t>> lod_indices;
	VeMesh::AABB local_aabb{};
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
	int specular_tex_idx = -1;
	int specular_color_tex_idx = -1;
	MaterialAlphaProps alpha_props;
	MaterialFactors factors;
	bool flip_tex_coord_v = false;
};

struct LoadedAssetData {
	std::vector<DecodedTexture> textures;
	std::vector<ProcessedMesh> meshes;
	std::vector<ProcessedMaterial> materials;
	std::vector<ModelNode> nodes;
	std::vector<std::pair<uint32_t, uint32_t>> parent_links;	// (child_index, parent_index)
	std::unordered_set<uint32_t> root_indices;
	std::vector<ExtractedLight> punctual_lights;
	std::vector<ExtractedLight> emissive_lights;
	std::vector<ExtractedCamera> cameras;
	std::unordered_map<int, uint32_t> gltf_to_loaded_idx;
	std::vector<VeAnimationClip> animation_clips;
	std::vector<ModelSkin> skins;
	EmbeddedImageCache embedded_images;

	LoadedAssetData() = default;
	LoadedAssetData(LoadedAssetData&&) = default;
	LoadedAssetData& operator=(LoadedAssetData&&) = default;
	LoadedAssetData(const LoadedAssetData&) = delete;
	LoadedAssetData& operator=(const LoadedAssetData&) = delete;
};

// Thread-safe progress tracking for async loading.
// Each texture decode, texture upload, mesh process, mesh upload, and material
// creation counts as one item. Progress = completed_items / total_items.
struct LoadProgress {
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
