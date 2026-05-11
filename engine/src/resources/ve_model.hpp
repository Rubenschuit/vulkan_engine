/* VeModel - scene graph container for glTF models.
 * Loads glTF, creates VeMesh and VeMaterial resources, builds node hierarchy.
 * Nodes are stored as ModelNode structs; addToScene creates entities in a Registry.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_material.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_animation_clip.hpp"
#include "scene/ve_registry.hpp"
#include <atomic>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ve {

class VeDescriptorPool;
class VeDescriptorSetLayout;
struct LoadedAssetData;
struct LoadProgress;

struct ModelNode {
	std::string name;
	glm::vec3 translation{0.f};
	glm::quat rotation{1.f, 0.f, 0.f, 0.f};
	glm::vec3 scale{1.f};
	int mesh_idx = -1;
	int material_idx = -1;
	int skin_idx = -1;
};

struct ModelSkin {
	std::vector<int> joint_node_indices;
	std::vector<glm::mat4> inverse_bind_matrices;
	int skeleton_root_node = -1;
};

class VENGINE_API VeModel {
public:
	// Load glTF from path, create meshes and node hierarchy
	// pool and material_layout can be null for models without textures
	// extract_lights: parse KHR_lights_punctual + emissive-as-lights when addToScene is used
	// flip_tex_coord_v: when true, materials use flipped v for tex coords
	static std::unique_ptr<VeModel> load(VeResourceManager& resource_manager,
	                                    const std::filesystem::path& model_path,
	                                    VeDescriptorPool* pool = nullptr,
	                                    VeDescriptorSetLayout* material_layout = nullptr,
	                                    bool extract_lights = false,
	                                    bool flip_tex_coord_v = false);

	~VeModel();

	VeModel(const VeModel&) = delete;
	VeModel& operator=(const VeModel&) = delete;

	// Add all loaded nodes to a Registry as entities with components and hierarchy.
	// Can only be called once per model; m_nodes is cleared after use.
	void addToScene(Registry& registry,
	                const glm::vec3& root_translation,
	                const glm::vec3& root_rotation,
	                const glm::vec3& root_scale);

	// Load a glTF and extract the first mesh+material resource handles.
	// Returns nullopt if the model contains no valid mesh.
	struct SingleMeshData {
		ResourceHandle<VeMesh> mesh;
		ResourceHandle<VeMaterial> material;
	};
	static std::optional<SingleMeshData> loadSingleMesh(
		VeResourceManager& resource_manager,
		const std::filesystem::path& model_path,
		bool flip_tex_coord_v = false);

	// Lights extracted from glTF (KHR_lights_punctual) or from emissive materials. 
	// Applied when addToScene is used.
	enum class ExtractedLightType { Point, Directional, Spot };
	struct ExtractedLight {
		ExtractedLightType type = ExtractedLightType::Point;
		glm::vec3 position{0.f};
		glm::vec3 direction{0.f, 0.f, -1.f};
		glm::vec3 color{1.f};
		float intensity = 1.f;
		float range = 0.f;  // 0 = no range limit
		float inner_cone_angle = 0.f;                    // half-angle in radians (glTF default: 0)
		float outer_cone_angle = glm::radians(45.0f);    // half-angle in radians (glTF default: pi/4)
		std::string name;
		int node_idx = -1;  // glTF node that produced this light (-1 = unknown)
	};
	const std::vector<ExtractedLight>& getPunctualLights() const { return m_punctual_lights; }
	const std::vector<ExtractedLight>& getEmissiveLights() const { return m_emissive_lights; }

	// Cameras extracted from glTF (perspective or orthographic). Applied when addToScene is used
	// by attaching a CameraComponent to the entity created for the source glTF node.
	struct ExtractedCamera {
		bool perspective = true;
		float yfov_radians = glm::radians(55.0f);
		float ortho_size = 10.0f;
		float znear = 0.1f;
		float zfar = 1000.0f;
		std::string name;
		int node_idx = -1;
	};
	const std::vector<ExtractedCamera>& getCameras() const { return m_cameras; }

	// CPU-only glTF loading: parses, decodes textures, processes meshes.
	// No vulkan calls; it is thread-safe.
	static LoadedAssetData loadFromGltfCpu(
		const std::filesystem::path& model_path,
		bool extract_lights, bool flip_tex_coord_v,
		LoadProgress& progress);

	// Construct a VeModel from a fully-uploaded LoadedAssetData (GPU handles already created).
	static std::unique_ptr<VeModel> fromLoadedData(
		LoadedAssetData&& data,
		std::vector<ResourceHandle<VeMesh>>& mesh_handles,
		std::vector<ResourceHandle<VeMaterial>>& material_handles);

	VeModel();

private:

	void loadFromGltf(const std::filesystem::path& model_path, VeResourceManager& resource_manager,
	                  VeDescriptorPool* pool, VeDescriptorSetLayout* material_layout,
	                  bool extract_lights, bool flip_tex_coord_v = false);

	std::vector<ModelNode> m_nodes;
	std::vector<std::pair<uint32_t, uint32_t>> m_parent_links;  // (child_index, parent_index)
	std::unordered_set<uint32_t> m_root_indices;

	std::vector<ResourceHandle<VeMesh>> m_mesh_handles;
	std::vector<ResourceHandle<VeMaterial>> m_material_handles;
	std::vector<ExtractedLight> m_punctual_lights;
	std::vector<ExtractedLight> m_emissive_lights;
	std::vector<ExtractedCamera> m_cameras;
	std::unordered_map<int, uint32_t> m_gltf_to_loaded_idx;
	std::vector<std::shared_ptr<VeAnimationClip>> m_animation_clips;
	std::vector<ModelSkin> m_skins;
};

} // namespace ve
