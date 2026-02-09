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
	static std::unique_ptr<VeModel> load(VeResourceManager& resource_manager,
	                                    const std::filesystem::path& model_path,
	                                    VeDescriptorPool* pool = nullptr,
	                                    VeDescriptorSetLayout* material_layout = nullptr);

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
	                                       const glm::vec3& scale);

	// Get all node IDs for iteration (e.g. to find root)
	const std::vector<VeGameObject>& getNodes() const { return m_nodes; }

	// Root node ID (first root added to scene)
	uint32_t getRootId() const { return m_root_id; }

	VeModel();

private:

	void loadFromGltf(const std::filesystem::path& model_path, VeResourceManager& resource_manager,
	                  VeDescriptorPool* pool, VeDescriptorSetLayout* material_layout);

	std::vector<VeGameObject> m_nodes;
	std::vector<std::pair<uint32_t, uint32_t>> m_parent_links;  // (child_id, parent_id)
	std::unordered_set<uint32_t> m_root_ids;
	uint32_t m_root_id{0};

	std::vector<ResourceHandle<VeMaterial>> m_material_handles;
};

} // namespace ve
