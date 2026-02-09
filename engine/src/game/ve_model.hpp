/* VeModel - scene graph container for glTF models.
 * Not a Resource - owns nodes (VeGameObjects) that make up the hierarchy.
 * Loads glTF, creates VeMesh resources for each primitive, builds node hierarchy.
 */
#pragma once
#include "ve_export.hpp"
#include "core/ve_device.hpp"
#include "core/ve_texture.hpp"
#include "core/ve_descriptors.hpp"
#include "core/ve_resource_manager.hpp"
#include "game/ve_mesh.hpp"
#include "game/ve_game_object.hpp"

#include "ve_scene.hpp"
#include <filesystem>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ve {

class VeDescriptorPool;
class VeDescriptorSetLayout;

struct Material {
	ResourceHandle<VeTexture> albedo_texture;
	ResourceHandle<VeTexture> normal_texture;
	ResourceHandle<VeTexture> metallic_roughness_texture;
	std::optional<vk::raii::DescriptorSet> descriptor_set;
};

class VENGINE_API VeModel {
public:
	// Load glTF from path, create meshes and node hierarchy
	// pool and material_layout can be null for models without textures (e.g. skybox cube)
	static std::unique_ptr<VeModel> load(VeDevice& device, VeResourceManager& resource_manager,
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
	static VeGameObject loadAsSingleObject(VeDevice& device, VeResourceManager& resource_manager,
	                                       const std::filesystem::path& model_path,
	                                       const glm::vec3& translation,
	                                       const glm::vec3& rotation,
	                                       const glm::vec3& scale);

	// Get all node IDs for iteration (e.g. to find root)
	const std::vector<VeGameObject>& getNodes() const { return m_nodes; }

	// Material descriptor set for PBR rendering (per-material)
	vk::raii::DescriptorSet& getMaterialDescriptorSet(uint32_t material_index);
	uint32_t getMaterialCount() const { return static_cast<uint32_t>(m_materials.size()); }
	bool hasTexturedMaterials() const { return m_has_textured_materials; }

	// Material alpha props from glTF (alphaMode, alphaCutoff, doubleSided). Returns default if index out of range.
	MaterialAlphaProps getMaterialAlphaProps(uint32_t material_index) const;

	// Root node ID (first root added to scene)
	uint32_t getRootId() const { return m_root_id; }

	VeModel(VeDevice& device);

private:

	void loadFromGltf(const std::filesystem::path& model_path, VeResourceManager& resource_manager,
	                  VeDescriptorPool* pool, VeDescriptorSetLayout* material_layout);
	void createPerMaterialTextures(VeResourceManager& resource_manager,
	                              const std::vector<std::filesystem::path>& albedo_paths,
	                              const std::vector<std::filesystem::path>& normal_paths,
	                              const std::vector<std::filesystem::path>& metallic_roughness_paths);
	void createPerMaterialDescriptorSets(VeDescriptorPool& pool, VeDescriptorSetLayout& set_layout);

	VeDevice& m_ve_device;
	std::vector<VeGameObject> m_nodes;
	std::vector<std::pair<uint32_t, uint32_t>> m_parent_links;  // (child_id, parent_id)
	std::unordered_set<uint32_t> m_root_ids;
	uint32_t m_root_id{0};

	std::vector<Material> m_materials;
	std::vector<MaterialAlphaProps> m_material_alpha_props;
	bool m_has_textured_materials{false};
};

} // namespace ve
