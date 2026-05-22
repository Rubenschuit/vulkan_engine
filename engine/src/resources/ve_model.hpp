/* VeModel - reusable post-load asset.
 * Holds GPU resource handles (meshes, materials) plus the engine-ready node
 * graph and extracted lights/cameras/skins/animation_clips. The same VeModel
 * can be added into a Registry multiple times via scene_instantiation.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_resource.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_animation_clip.hpp"
#include "resources/model_data.hpp"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ve {

struct LoadedAssetData;
struct UploadedHandles;

class VENGINE_API VeModel : public Resource {
public:
	VeModel(const std::string& resource_id,
	        LoadedAssetData&& data, UploadedHandles&& handles);
	~VeModel() override;

	VeModel(const VeModel&) = delete;
	VeModel& operator=(const VeModel&) = delete;

	const std::vector<ModelNode>& nodes() const { return m_nodes; }
	const std::vector<std::pair<uint32_t, uint32_t>>& parentLinks() const { return m_parent_links; }
	const std::unordered_set<uint32_t>& rootIndices() const { return m_root_indices; }
	const std::vector<ResourceHandle<VeMesh>>& meshHandles() const { return m_mesh_handles; }
	const std::vector<ResourceHandle<VeMaterial>>& materialHandles() const { return m_material_handles; }
	const std::vector<ExtractedLight>& punctualLights() const { return m_punctual_lights; }
	const std::vector<ExtractedLight>& emissiveLights() const { return m_emissive_lights; }
	const std::vector<ExtractedCamera>& cameras() const { return m_cameras; }
	const std::unordered_map<int, uint32_t>& gltfToLoadedIdx() const { return m_gltf_to_loaded_idx; }
	const std::vector<std::shared_ptr<VeAnimationClip>>& animationClips() const { return m_animation_clips; }
	const std::vector<ModelSkin>& skins() const { return m_skins; }

protected:
	bool doLoad() override { return true; }
	void doUnload() override;
	void emitUnloadingEvent(EventBus& bus) override;

private:
	std::vector<ModelNode> m_nodes;
	std::vector<std::pair<uint32_t, uint32_t>> m_parent_links;
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
