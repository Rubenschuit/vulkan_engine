/* VeModel - scene graph container for glTF models.
 * Not a Resource - owns nodes (VeGameObjects) that make up the hierarchy.
 * Loads glTF, creates VeMesh and VeMaterial resources, builds node hierarchy.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_material.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "scene/ve_game_object.hpp"
#include "scene/ve_scene.hpp"
#include <filesystem>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
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

	// Add all nodes to the scene (the unordered_map is used to store the game objects by their id)
	void addToScene(std::unordered_map<uint32_t, VeGameObject>& game_objects,
	                const glm::vec3& root_translation,
	                const glm::vec3& root_rotation,
	                const glm::vec3& root_scale);

	// Alternative: return a vector of objects instead of mutating a map. Caller can modify and merge.
	std::vector<VeGameObject> addToScene(const glm::vec3& root_translation,
	                                    const glm::vec3& root_rotation,
	                                    const glm::vec3& root_scale);

	// Load a simple single mesh model (quad, cube, etc.) and return a single GameObject with transform applied.
	// If no mesh found, create a new empty game object.
	static VeGameObject loadAsSingleObject(VeResourceManager& resource_manager,
	                                       const std::filesystem::path& model_path,
	                                       const glm::vec3& translation,
	                                       const glm::vec3& rotation,
	                                       const glm::vec3& scale,
	                                       bool flip_tex_coord_v = false);

	// Get all node IDs for iteration (e.g. to find root)
	const std::vector<VeGameObject>& getNodes() const { return m_nodes; }

	// Root node ID (first root added to scene)
	uint32_t getRootId() const { return m_root_id; }

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

	std::vector<VeGameObject> m_nodes;
	std::vector<std::pair<uint32_t, uint32_t>> m_parent_links;  // (child_id, parent_id)
	std::unordered_set<uint32_t> m_root_ids;
	uint32_t m_root_id{0};

	std::vector<ResourceHandle<VeMaterial>> m_material_handles;
	std::vector<ExtractedLight> m_punctual_lights;
	std::vector<ExtractedLight> m_emissive_lights;
};

} // namespace ve
