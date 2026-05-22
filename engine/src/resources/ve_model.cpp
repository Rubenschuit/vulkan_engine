#include "pch.hpp"
#include "resources/ve_model.hpp"
#include "resources/internal/asset_upload.hpp"
#include "resources/internal/gltf_loader.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "vulkan/ve_device.hpp"
#include "utils/ve_log.hpp"

namespace ve {

VeModel::VeModel(const std::string& resource_id,
                 LoadedAssetData&& data, UploadedHandles&& handles)
	: Resource(resource_id)
	, m_nodes(std::move(data.nodes))
	, m_parent_links(std::move(data.parent_links))
	, m_root_indices(std::move(data.root_indices))
	, m_mesh_handles(std::move(handles.meshes))
	, m_material_handles(std::move(handles.materials))
	, m_punctual_lights(std::move(data.punctual_lights))
	, m_emissive_lights(std::move(data.emissive_lights))
	, m_cameras(std::move(data.cameras))
	, m_gltf_to_loaded_idx(std::move(data.gltf_to_loaded_idx))
	, m_skins(std::move(data.skins))
{
	for (auto& c : data.animation_clips)
		m_animation_clips.push_back(std::make_shared<VeAnimationClip>(std::move(c)));
	setLoaded(true);
}

VeModel::~VeModel() {
	unload();
}

void VeModel::doUnload() {
	m_animation_clips.clear();
	m_skins.clear();
	m_cameras.clear();
	m_emissive_lights.clear();
	m_punctual_lights.clear();
	m_material_handles.clear();
	m_mesh_handles.clear();
	m_gltf_to_loaded_idx.clear();
	m_root_indices.clear();
	m_parent_links.clear();
	m_nodes.clear();
}

void VeModel::emitUnloadingEvent(EventBus& bus) {
	ResourceUnloadingEvent<VeModel> ev{};
	ev.resource = this;
	bus.emitImmediate(ev);
}

} // namespace ve