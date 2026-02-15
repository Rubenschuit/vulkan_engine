/* VeModel - scene graph container for glTF models.
 * Loads glTF, creates VeMesh and VeMaterial resources, builds node hierarchy.
 * Nodes are stored as lightweight LoadedNode structs; addToScene creates entities in a Registry.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_material.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "scene/ve_registry.hpp"
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

class VENGINE_API VeModel {
public:
	// Load glTF from path, create meshes and node hierarchy
	// pool and material_layout can be null for models without textures
	// extract_lights: if true, parse KHR_lights_punctual and add lights when addToScene is used
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

	// Lights extracted from glTF (KHR_lights_punctual) or from emissive materials. Applied when addToScene is used.
	enum class ExtractedLightType { Point, Directional };
	struct ExtractedLight {
		ExtractedLightType type = ExtractedLightType::Point;
		glm::vec3 position{0.f};
		glm::vec3 direction{0.f, 0.f, -1.f};
		glm::vec3 color{1.f};
		float intensity = 1.f;
		float range = 0.f;  // 0 = no range limit
		std::string name;
	};
	const std::vector<ExtractedLight>& getPunctualLights() const { return m_punctual_lights; }
	const std::vector<ExtractedLight>& getEmissiveLights() const { return m_emissive_lights; }

	VeModel();

private:

	void loadFromGltf(const std::filesystem::path& model_path, VeResourceManager& resource_manager,
	                  VeDescriptorPool* pool, VeDescriptorSetLayout* material_layout,
	                  bool extract_lights, bool flip_tex_coord_v = false);

	// Lightweight node data from glTF parsing
	struct LoadedNode {
		uint32_t id = 0;
		std::string name;
		glm::vec3 translation{0.f};
		glm::quat rotation{1.f, 0.f, 0.f, 0.f};
		glm::vec3 scale{1.f};
		ResourceHandle<VeMesh> mesh;
		ResourceHandle<VeMaterial> material;
	};

	std::vector<LoadedNode> m_nodes;
	std::vector<std::pair<uint32_t, uint32_t>> m_parent_links;  // (child_id, parent_id)
	std::unordered_set<uint32_t> m_root_ids;
	uint32_t m_root_id{0};

	std::vector<ResourceHandle<VeMaterial>> m_material_handles;
	std::vector<ExtractedLight> m_punctual_lights;
	std::vector<ExtractedLight> m_emissive_lights;

	static uint32_t s_next_node_id;
};

} // namespace ve
