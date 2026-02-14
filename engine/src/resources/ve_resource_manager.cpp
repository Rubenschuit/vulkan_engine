#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_texture.hpp"
#include "vulkan/ve_descriptors.hpp"

namespace ve {

// ---------------------------------------------------------------------------
// ResourceHandle implementation
// ---------------------------------------------------------------------------
template <typename T>
ResourceHandle<T>::ResourceHandle(const std::string& resource_id, VeResourceManager* manager)
	: m_resource_id(resource_id), m_manager(manager) {
}

template <typename T>
ResourceHandle<T>::~ResourceHandle() {
	release();
}

// Copy constructor
template <typename T>
ResourceHandle<T>::ResourceHandle(const ResourceHandle& other)
	: m_resource_id(other.m_resource_id), m_manager(other.m_manager) {
	addRef();
}

// Copy assignment operator
template <typename T>
ResourceHandle<T>& ResourceHandle<T>::operator=(const ResourceHandle& other) {
	if (this != &other) {
		release();
		m_resource_id = other.m_resource_id;
		m_manager = other.m_manager;
		addRef();
	}
	return *this;
}

// Move constructor
template <typename T>
ResourceHandle<T>::ResourceHandle(ResourceHandle&& other) noexcept
	: m_resource_id(std::move(other.m_resource_id)), m_manager(other.m_manager) {
	other.m_manager = nullptr;
}

// Move assignment operator
template <typename T>
ResourceHandle<T>& ResourceHandle<T>::operator=(ResourceHandle&& other) noexcept {
	if (this != &other) {
		release();
		m_resource_id = std::move(other.m_resource_id);
		m_manager = other.m_manager;
		other.m_manager = nullptr;
	}
	return *this;
}

// Get the resource pointer or nullptr if not found.
template <typename T>
T* ResourceHandle<T>::get() const {
	return m_manager ? m_manager->getResource<T>(m_resource_id) : nullptr;
}

// Check if the resource is valid.
template <typename T>
bool ResourceHandle<T>::isValid() const {
	return m_manager && m_manager->hasResource<T>(m_resource_id);
}

// Get the resource id.
template <typename T>
const std::string& ResourceHandle<T>::getId() const {
	return m_resource_id;
}

// Increment the reference count.
template <typename T>
void ResourceHandle<T>::addRef() const {
	if (m_manager)
		m_manager->addRef<T>(m_resource_id);
}

// Decrement the reference count.
// If reference count reaches 0, the resource is unloaded.
template <typename T>
void ResourceHandle<T>::release() const {
	if (m_manager) {
		m_manager->release<T>(m_resource_id);
		m_manager = nullptr;
	}
}

// ---------------------------------------------------------------------------
// VeResourceManager implementation
// ---------------------------------------------------------------------------

VeResourceManager::VeResourceManager(VeDevice& device) : m_device(device) {}

VeResourceManager::~VeResourceManager() {
    //unloadAll();
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
}

template <typename T>
ResourceHandle<T> VeResourceManager::load(const std::string& resource_id) {
	static_assert(std::is_base_of_v<Resource, T>, "T must derive from Resource");

	auto type_idx = typeid(T).hash_code();
	auto& type_resources = m_resources[type_idx];
	auto it = type_resources.find(resource_id);

	if (it != type_resources.end()) {
		m_ref_counts[type_idx][resource_id]++;
		return ResourceHandle<T>(resource_id, this);
	}

	auto resource = std::make_shared<T>(m_device, resource_id);
	if (!resource->load()) {
		return ResourceHandle<T>();
	}

	type_resources[resource_id] = std::move(resource);
	m_ref_counts[type_idx][resource_id] = 1;
	return ResourceHandle<T>(resource_id, this);
}

template <typename T>
T* VeResourceManager::getResource(const std::string& resource_id) const {
	auto type_idx = typeid(T).hash_code();
	auto type_it = m_resources.find(type_idx);
	if (type_it == m_resources.end())
		return nullptr;

	auto it = type_it->second.find(resource_id);
	if (it == type_it->second.end())
		return nullptr;

	return static_cast<T*>(it->second.get());
}

template <typename T>
bool VeResourceManager::hasResource(const std::string& resource_id) const {
	auto type_idx = typeid(T).hash_code();
	auto type_it = m_resources.find(type_idx);
	if (type_it == m_resources.end())
		return false;
	return type_it->second.find(resource_id) != type_it->second.end();
}

template <typename T>
void VeResourceManager::addRef(const std::string& resource_id) {
	auto type_idx = typeid(T).hash_code();
	auto& type_refs = m_ref_counts[type_idx];
	auto it = type_refs.find(resource_id);
	if (it != type_refs.end())
		it->second++;
}

// Decrement the reference count.
// If reference count reaches 0, the resource is unloaded.
// If resource not found, do nothing.
template <typename T>
void VeResourceManager::release(const std::string& resource_id) {
	auto type_idx = typeid(T).hash_code();
	auto& type_refs = m_ref_counts[type_idx];
	auto it = type_refs.find(resource_id);
	if (it == type_refs.end())
		return; // Resource not found.

	it->second--; // Decrement reference count.
	if (it->second <= 0) { // If reference count reaches 0, unload resource.
		auto& type_resources = m_resources[type_idx];
		auto resource_it = type_resources.find(resource_id);
		if (resource_it != type_resources.end()) {
			resource_it->second->unload();
			type_resources.erase(resource_it);
		}
		type_refs.erase(it);
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
	                                            alpha_props, factors, pool, layout, flip_tex_coord_v);
	if (!resource->load()) {
		return ResourceHandle<VeMaterial>();
	}
	type_resources[resource_id] = std::move(resource);
	m_ref_counts[type_idx][resource_id] = 1;
	return ResourceHandle<VeMaterial>(resource_id, this);
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
