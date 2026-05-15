#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_texture.hpp"
#include "resources/loaded_asset_data.hpp"
#include "rendering/culling/meshlet_data.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "ve_config.hpp"

#include <cassert>

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
                                                             const std::filesystem::path& albedo_path,
                                                             const std::filesystem::path& normal_path,
                                                             const std::filesystem::path& metallic_roughness_path,
                                                             const std::filesystem::path& occlusion_path,
                                                             const std::filesystem::path& emissive_path,
                                                             const std::filesystem::path& specular_path,
                                                             const std::filesystem::path& specular_color_path,
                                                             MaterialAlphaProps alpha_props,
                                                             MaterialFactors factors,
                                                             VeDescriptorPool* pool,
                                                             VeDescriptorSetLayout* layout,
                                                             bool flip_tex_coord_v) {
	auto type_idx = typeid(VeMaterial).hash_code();
	auto& type_resources = m_resources[type_idx];
	auto it = type_resources.find(resource_id);

	if (it != type_resources.end()) {
		m_ref_counts[type_idx][resource_id]++;
		return ResourceHandle<VeMaterial>(resource_id, this);
	}

	auto resource = std::make_shared<VeMaterial>(*this, resource_id,
	                                            albedo_path, normal_path, metallic_roughness_path,
	                                            occlusion_path, emissive_path,
	                                            specular_path, specular_color_path,
	                                            alpha_props, factors, pool, layout, flip_tex_coord_v);
	if (!resource->load()) {
		return ResourceHandle<VeMaterial>();
	}
	type_resources[resource_id] = std::move(resource);
	m_ref_counts[type_idx][resource_id] = 1;
	return ResourceHandle<VeMaterial>(resource_id, this);
}

ResourceHandle<VeMesh> VeResourceManager::createMeshFromData(
	const std::string& resource_id, const ProcessedMesh& data) {
	auto type_idx = typeid(VeMesh).hash_code();
	auto& type_resources = m_resources[type_idx];
	auto it = type_resources.find(resource_id);

	if (it != type_resources.end()) {
		m_ref_counts[type_idx][resource_id]++;
		return ResourceHandle<VeMesh>(resource_id, this);
	}

	auto resource = std::make_shared<VeMesh>(m_device, data);
	type_resources[resource_id] = std::move(resource);
	m_ref_counts[type_idx][resource_id] = 1;
	return ResourceHandle<VeMesh>(resource_id, this);
}

// ---------------------------------------------------------------------------
// Explicit instantiations (VENGINE_API for DLL export)
// ---------------------------------------------------------------------------
template class VENGINE_API ResourceHandle<VeMesh>;
template class VENGINE_API ResourceHandle<VeTexture>;
template class VENGINE_API ResourceHandle<VeMaterial>;

template VENGINE_API ResourceHandle<VeTexture> VeResourceManager::load<VeTexture>(const std::string&);

template VeTexture* VeResourceManager::getResource<VeTexture>(const std::string&) const;
template VeMesh* VeResourceManager::getResource<VeMesh>(const std::string&) const;
template VeMaterial* VeResourceManager::getResource<VeMaterial>(const std::string&) const;

template bool VeResourceManager::hasResource<VeTexture>(const std::string&) const;
template bool VeResourceManager::hasResource<VeMesh>(const std::string&) const;
template bool VeResourceManager::hasResource<VeMaterial>(const std::string&) const;

template void VeResourceManager::addRef<VeTexture>(const std::string&);
template void VeResourceManager::addRef<VeMesh>(const std::string&);
template void VeResourceManager::addRef<VeMaterial>(const std::string&);

template void VeResourceManager::release<VeTexture>(const std::string&);
template void VeResourceManager::release<VeMesh>(const std::string&);
template void VeResourceManager::release<VeMaterial>(const std::string&);

} // namespace ve
