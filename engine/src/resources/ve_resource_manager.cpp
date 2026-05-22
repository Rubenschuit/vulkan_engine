#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_texture.hpp"
#include "resources/ve_model.hpp"
#include "resources/internal/loaded_asset_data.hpp"
#include "resources/internal/asset_upload.hpp"
#include "resources/internal/upload_context.hpp"
#include "resources/internal/gltf_loader.hpp"
#include "rendering/culling/meshlet_data.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "utils/ve_log.hpp"
#include "ve_config.hpp"

#include <cassert>
#include <cstddef>
#include <limits>

namespace ve {

// ---------------------------------------------------------------------------
// VeResourceManager implementation (non-template members; template definitions
// live in the header so foreign TUs / tests can instantiate them).
// ---------------------------------------------------------------------------

VeResourceManager::VeResourceManager(VeDevice& device, EventBus& event_bus)
	: m_device(device), m_event_bus(event_bus) {}

VeResourceManager::~VeResourceManager() {
	flushPendingUnloads();
	assert(m_pending_unloads.empty());
}

// Unload all resources managed by this manager.
void VeResourceManager::unloadAll() {
	for (auto& [type, type_resources] : m_resources) {
		for (auto& [id, resource] : type_resources) {
			resource->unload();
		}
		type_resources.clear();
	}
	m_ref_counts.clear();
	m_pending_unloads.clear();
	m_latest_retire_frame.clear();
}

void VeResourceManager::processPendingEntry(const PendingUnload& entry) {
	// Check this entry hasn't been superseded by a later push for the same id.
	auto latest_type_it = m_latest_retire_frame.find(entry.type_idx);
	if (latest_type_it == m_latest_retire_frame.end())
		return;
	auto latest_it = latest_type_it->second.find(entry.resource_id);
	if (latest_it == latest_type_it->second.end())
		return;
	if (latest_it->second != entry.retire_frame)
		return;

	auto type_refs_it = m_ref_counts.find(entry.type_idx);
	if (type_refs_it == m_ref_counts.end())
		return;
	auto ref_it = type_refs_it->second.find(entry.resource_id);
	if (ref_it == type_refs_it->second.end())
		return; // earlier retirement already destroyed it
	if (ref_it->second > 0)
		return; // rescued by load() during the deferred window

	auto type_res_it = m_resources.find(entry.type_idx);
	if (type_res_it != m_resources.end()) {
		auto res_it = type_res_it->second.find(entry.resource_id);
		if (res_it != type_res_it->second.end()) {
			// Notify subscribers (e.g. MaterialSSBOManager, BindlessTextureRegistry)
			res_it->second->emitUnloadingEvent(m_event_bus);

			res_it->second->unload();
			type_res_it->second.erase(res_it);
		}
	}
	type_refs_it->second.erase(ref_it);
	latest_type_it->second.erase(latest_it);
}

void VeResourceManager::tickFrame() {
	++m_current_frame;
	while (!m_pending_unloads.empty()) {
		if (m_pending_unloads.front().retire_frame > m_current_frame)
			break;
		PendingUnload local = m_pending_unloads.front();
		m_pending_unloads.pop_front();
		processPendingEntry(local);
	}
}

void VeResourceManager::flushPendingUnloads() {
	while (!m_pending_unloads.empty()) {
		PendingUnload local = m_pending_unloads.front();
		m_pending_unloads.pop_front();
		processPendingEntry(local);
	}
}

ResourceHandle<VeMesh> VeResourceManager::createMesh(const std::string& resource_id,
                                                    const std::vector<VeMesh::Vertex>& vertices,
                                                    const std::vector<uint32_t>& indices) {
	auto type_idx = typeid(VeMesh).hash_code();
	auto& type_resources = m_resources[type_idx];
	auto it = type_resources.find(resource_id);

	// If resource already exists, increment reference count.
	if (it != type_resources.end()) {
		m_ref_counts[type_idx][resource_id]++;
		return ResourceHandle<VeMesh>(resource_id, this);
	}

	// Create new resource.
	auto resource = std::make_shared<VeMesh>(m_device, resource_id, vertices, indices);
	resource->setMeshletData(VeMesh::buildMeshletData(vertices, indices));
	type_resources[resource_id] = std::move(resource);
	m_ref_counts[type_idx][resource_id] = 1;
	return ResourceHandle<VeMesh>(resource_id, this);
}

ResourceHandle<VeMesh> VeResourceManager::createMesh(const std::string& resource_id,
                                                    const std::vector<VeMesh::Vertex>& vertices,
                                                    const std::vector<uint32_t>& indices,
                                                    const std::vector<std::vector<uint32_t>>& lod_indices) {
	auto type_idx = typeid(VeMesh).hash_code();
	auto& type_resources = m_resources[type_idx];
	auto it = type_resources.find(resource_id);

	if (it != type_resources.end()) {
		m_ref_counts[type_idx][resource_id]++;
		return ResourceHandle<VeMesh>(resource_id, this);
	}

	auto resource = std::make_shared<VeMesh>(m_device, resource_id, vertices, indices, lod_indices);
	resource->setMeshletData(VeMesh::buildMeshletData(vertices, indices, lod_indices));
	type_resources[resource_id] = std::move(resource);
	m_ref_counts[type_idx][resource_id] = 1;
	return ResourceHandle<VeMesh>(resource_id, this);
}

ResourceHandle<VeMaterial> VeResourceManager::createMaterial(const std::string& resource_id,
                                                             MaterialTextures textures,
                                                             MaterialAlphaProps alpha_props,
                                                             MaterialFactors factors,
                                                             bool flip_tex_coord_v) {
	auto type_idx = typeid(VeMaterial).hash_code();
	auto& type_resources = m_resources[type_idx];
	auto it = type_resources.find(resource_id);

	if (it != type_resources.end()) {
		m_ref_counts[type_idx][resource_id]++;
		return ResourceHandle<VeMaterial>(resource_id, this);
	}

	// Substitute defaults for any unbound slots
	if (!textures.albedo.isValid())
		textures.albedo = load<VeTexture>("default_albedo");
	if (!textures.normal.isValid())
		textures.normal = load<VeTexture>("default_normal");
	if (!textures.metallic_roughness.isValid())
		textures.metallic_roughness = load<VeTexture>("default_metallic_roughness");
	if (!textures.occlusion.isValid())
		textures.occlusion = load<VeTexture>("default_occlusion");
	if (!textures.emissive.isValid())
		textures.emissive = load<VeTexture>("default_emissive");
	if (!textures.specular.isValid())
		textures.specular = load<VeTexture>("default_specular");
	if (!textures.specular_color.isValid())
		textures.specular_color = load<VeTexture>("default_specular_color");

	auto resource = std::make_shared<VeMaterial>(resource_id, std::move(textures),
	                                             alpha_props, factors, flip_tex_coord_v);
	type_resources[resource_id] = std::move(resource);
	m_ref_counts[type_idx][resource_id] = 1;
	return ResourceHandle<VeMaterial>(resource_id, this);
}

ResourceHandle<VeMesh> VeResourceManager::createMeshFromData(
	const std::string& resource_id, ProcessedMesh& data, UploadContext& ctx) {
	auto type_idx = typeid(VeMesh).hash_code();
	auto& type_resources = m_resources[type_idx];
	auto it = type_resources.find(resource_id);

	if (it != type_resources.end()) {
		m_ref_counts[type_idx][resource_id]++;
		return ResourceHandle<VeMesh>(resource_id, this);
	}

	auto resource = std::make_shared<VeMesh>(m_device, data, ctx);
	type_resources[resource_id] = std::move(resource);
	m_ref_counts[type_idx][resource_id] = 1;
	return ResourceHandle<VeMesh>(resource_id, this);
}

ResourceHandle<VeTexture> VeResourceManager::createTextureFromData(
	const std::string& resource_id, const DecodedTexture& data, UploadContext& ctx) {
	auto type_idx = typeid(VeTexture).hash_code();
	auto& type_resources = m_resources[type_idx];
	auto it = type_resources.find(resource_id);

	if (it != type_resources.end()) {
		m_ref_counts[type_idx][resource_id]++;
		return ResourceHandle<VeTexture>(resource_id, this);
	}

	auto resource = std::make_shared<VeTexture>(m_device, resource_id, data, ctx);
	if (!resource->isLoaded())
		return ResourceHandle<VeTexture>();
	type_resources[resource_id] = std::move(resource);
	m_ref_counts[type_idx][resource_id] = 1;
	return ResourceHandle<VeTexture>(resource_id, this);
}

ResourceHandle<VeMesh> VeResourceManager::loadMesh(const std::filesystem::path& gltf_path) {
	return ve::gltf::loadFirstMesh(*this, gltf_path);
}

ResourceHandle<VeModel> VeResourceManager::loadModel(const std::filesystem::path& gltf_path,
                                                     bool extract_lights, bool flip_tex_coord_v) {
	std::string key = gltf_path.lexically_normal().generic_string();
	auto type_idx = typeid(VeModel).hash_code();
	auto& type_resources = m_resources[type_idx];
	auto it = type_resources.find(key);
	if (it != type_resources.end()) {
		m_ref_counts[type_idx][key]++;
		return ResourceHandle<VeModel>(key, this);
	}

	LoadProgress progress;
	GpuCaps caps{ m_device.supportsBC(), m_device.supportsASTC() };
	LoadedAssetData data = ve::gltf::load(gltf_path, extract_lights, flip_tex_coord_v, progress, caps);
	if (progress.cpu_failed.load()) {
		VE_LOGE("loadModel: CPU load failed for " << gltf_path);
		return {};
	}

	UploadedHandles handles;
	UploadCursor cursor;
	{
		SyncUploadScope scope(m_device);
		uploadLoadedAssetStep(*this, data, cursor, handles,
		                      std::numeric_limits<uint32_t>::max(),
		                      std::numeric_limits<size_t>::max(),
		                      scope.ctx);
	}

	auto model = std::make_shared<VeModel>(key, std::move(data), std::move(handles));
	type_resources[key] = std::move(model);
	m_ref_counts[type_idx][key] = 1;
	return ResourceHandle<VeModel>(key, this);
}

// ---------------------------------------------------------------------------
// Explicit instantiations (VENGINE_API for DLL export)
// ---------------------------------------------------------------------------
template class VENGINE_API ResourceHandle<VeMesh>;
template class VENGINE_API ResourceHandle<VeTexture>;
template class VENGINE_API ResourceHandle<VeMaterial>;
template class VENGINE_API ResourceHandle<VeModel>;

template VENGINE_API ResourceHandle<VeTexture> VeResourceManager::load<VeTexture>(const std::string&);

template VeTexture* VeResourceManager::getResource<VeTexture>(const std::string&) const;
template VeMesh* VeResourceManager::getResource<VeMesh>(const std::string&) const;
template VeMaterial* VeResourceManager::getResource<VeMaterial>(const std::string&) const;
template VeModel* VeResourceManager::getResource<VeModel>(const std::string&) const;

template bool VeResourceManager::hasResource<VeTexture>(const std::string&) const;
template bool VeResourceManager::hasResource<VeMesh>(const std::string&) const;
template bool VeResourceManager::hasResource<VeMaterial>(const std::string&) const;
template bool VeResourceManager::hasResource<VeModel>(const std::string&) const;

template void VeResourceManager::addRef<VeTexture>(const std::string&);
template void VeResourceManager::addRef<VeMesh>(const std::string&);
template void VeResourceManager::addRef<VeMaterial>(const std::string&);
template void VeResourceManager::addRef<VeModel>(const std::string&);

template void VeResourceManager::release<VeTexture>(const std::string&);
template void VeResourceManager::release<VeMesh>(const std::string&);
template void VeResourceManager::release<VeMaterial>(const std::string&);
template void VeResourceManager::release<VeModel>(const std::string&);

} // namespace ve
