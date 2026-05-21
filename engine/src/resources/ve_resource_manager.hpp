/* Resource manager and handle for caching and lifetime management of resources
 * such as models and textures. TODO: add support for other resource types suchh as audio and shaders.
 * Based on:
 * https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Engine_Architecture/04_resource_management.html
 */
#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "resources/ve_resource.hpp"
#include "vulkan/ve_device.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material_properties.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <typeinfo>
#include <unordered_map>


namespace ve {

// Forward declarations
class VeResourceManager;
class VeMesh;
class VeMaterial;
class VeTexture;
class EventBus;
struct DecodedTexture;
struct ProcessedMesh;

/* RAII handle that keeps a resource loaded.
 * Copy increments ref count, destroy decrements. When ref count hits 0, resource is unloaded.
 */
template <typename T>
class ResourceHandle {
public:
	ResourceHandle() = default;
	ResourceHandle(const std::string& resource_id, VeResourceManager* manager);
	~ResourceHandle();

	ResourceHandle(const ResourceHandle& other);
	ResourceHandle& operator=(const ResourceHandle& other);
	ResourceHandle(ResourceHandle&& other) noexcept;
	ResourceHandle& operator=(ResourceHandle&& other) noexcept;

	T* get() const { return m_cached; }
	bool isValid() const { return m_cached != nullptr; }
	const std::string& getId() const { return m_resource_id; }

	T* operator->() const { return m_cached; }
	explicit operator bool() const { return isValid(); }

private:
	void addRef() const;
	void release() const;

	std::string m_resource_id;
	mutable VeResourceManager* m_manager = nullptr;
	mutable T* m_cached = nullptr;
};



class VENGINE_API VeResourceManager {
public:
	VeResourceManager(VeDevice& device, EventBus& event_bus);

	~VeResourceManager();

	template <typename T>
	ResourceHandle<T> load(const std::string& resource_id);

	template <typename T>
	T* getResource(const std::string& resource_id) const;

	template <typename T>
	bool hasResource(const std::string& resource_id) const;

	template <typename T>
	void addRef(const std::string& resource_id);

	template <typename T>
	void release(const std::string& resource_id);

	// Create VeMesh from vertex data
	ResourceHandle<VeMesh> createMesh(const std::string& resource_id,
	                                  const std::vector<VeMesh::Vertex>& vertices,
	                                  const std::vector<uint32_t>& indices);
	// Create VeMesh with additional LOD index buffers
	ResourceHandle<VeMesh> createMesh(const std::string& resource_id,
	                                  const std::vector<VeMesh::Vertex>& vertices,
	                                  const std::vector<uint32_t>& indices,
	                                  const std::vector<std::vector<uint32_t>>& lod_indices);

	ResourceHandle<VeMaterial> createMaterial(const std::string& resource_id,
	                                         const std::filesystem::path& albedo_path,
	                                         const std::filesystem::path& normal_path,
	                                         const std::filesystem::path& metallic_roughness_path,
	                                         const std::filesystem::path& occlusion_path,
	                                         const std::filesystem::path& emissive_path,
	                                         const std::filesystem::path& specular_path,
	                                         const std::filesystem::path& specular_color_path,
	                                         MaterialAlphaProps alpha_props,
	                                         MaterialFactors factors,
	                                         bool flip_tex_coord_v = false);

	ResourceHandle<VeMesh> createMeshFromData(const std::string& resource_id,
	                                          const ProcessedMesh& data);

	// Forcibly unload all resources regardless of reference count.
	void unloadAll();

	// Advance the manager's frame counter and handle any pending unloads whose
	// retire_frame has been reached.
	//
	// CONTRACT: Called exactly once per frame, after waitForCurrentFence() in
	// the renderer. Called from VeRenderer::beginFrame(); do not call elsewhere.
	void tickFrame();

	// Handle all pending unloads immediately. Caller must guarantee GPU idle.
	void flushPendingUnloads();

	size_t pendingUnloadCount() const { return m_pending_unloads.size(); }

	// For unit testing.
	template <typename T>
	ResourceHandle<T> registerExisting(const std::string& id, std::shared_ptr<T> resource) {
		static_assert(std::is_base_of_v<Resource, T>, "T must derive from Resource");
		auto type_idx = typeid(T).hash_code();
		m_resources[type_idx][id] = std::move(resource);
		m_ref_counts[type_idx][id] = 1;
		return ResourceHandle<T>(id, this);
	}

	VeDevice& getDevice() { return m_device; }
	const VeDevice& getDevice() const { return m_device; }

private:
	VeDevice& m_device;
	EventBus& m_event_bus;
	// Map of resource type to map of resource id to resource.
	// Used to store all resources.
	std::unordered_map<size_t, std::unordered_map<std::string, std::shared_ptr<Resource>>> m_resources;
	// Map of resource type to map of resource id to reference count.
	// Used to track the number of references to each resource.
	std::unordered_map<size_t, std::unordered_map<std::string, int>> m_ref_counts;

	// Resources whose refcount hit zero stay in m_resources until retire_frame is
	// reached. tickFrame re-checks refcount: still 0 => doUnload + erase; >0 => rescued, pop.
	struct PendingUnload {
		uint64_t retire_frame;
		size_t type_idx;
		std::string resource_id;
	};
	std::deque<PendingUnload> m_pending_unloads;
	std::unordered_map<size_t, std::unordered_map<std::string, uint64_t>> m_latest_retire_frame;
	uint64_t m_current_frame = 0;

	void processPendingEntry(const PendingUnload& entry);

	template <typename T>
	friend class ResourceHandle;
};

// ---------------------------------------------------------------------------
// Template definitions
// ---------------------------------------------------------------------------

template <typename T>
inline ResourceHandle<T>::ResourceHandle(const std::string& resource_id, VeResourceManager* manager)
	: m_resource_id(resource_id), m_manager(manager),
	  m_cached(manager ? manager->getResource<T>(resource_id) : nullptr) {
}

template <typename T>
inline ResourceHandle<T>::~ResourceHandle() {
	release();
}

template <typename T>
inline ResourceHandle<T>::ResourceHandle(const ResourceHandle& other)
	: m_resource_id(other.m_resource_id), m_manager(other.m_manager), m_cached(other.m_cached) {
	addRef();
}

template <typename T>
inline ResourceHandle<T>& ResourceHandle<T>::operator=(const ResourceHandle& other) {
	if (this != &other) {
		release();
		m_resource_id = other.m_resource_id;
		m_manager = other.m_manager;
		m_cached = other.m_cached;
		addRef();
	}
	return *this;
}

template <typename T>
inline ResourceHandle<T>::ResourceHandle(ResourceHandle&& other) noexcept
	: m_resource_id(std::move(other.m_resource_id)), m_manager(other.m_manager), m_cached(other.m_cached) {
	other.m_manager = nullptr;
	other.m_cached = nullptr;
}

template <typename T>
inline ResourceHandle<T>& ResourceHandle<T>::operator=(ResourceHandle&& other) noexcept {
	if (this != &other) {
		release();
		m_resource_id = std::move(other.m_resource_id);
		m_manager = other.m_manager;
		m_cached = other.m_cached;
		other.m_manager = nullptr;
		other.m_cached = nullptr;
	}
	return *this;
}

template <typename T>
inline void ResourceHandle<T>::addRef() const {
	if (m_manager)
		m_manager->addRef<T>(m_resource_id);
}

template <typename T>
inline void ResourceHandle<T>::release() const {
	if (m_manager) {
		m_manager->release<T>(m_resource_id);
		m_manager = nullptr;
		m_cached = nullptr;
	}
}

template <typename T>
inline ResourceHandle<T> VeResourceManager::load(const std::string& resource_id) {
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
inline T* VeResourceManager::getResource(const std::string& resource_id) const {
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
inline bool VeResourceManager::hasResource(const std::string& resource_id) const {
	auto type_idx = typeid(T).hash_code();
	auto type_it = m_resources.find(type_idx);
	if (type_it == m_resources.end())
		return false;
	return type_it->second.find(resource_id) != type_it->second.end();
}

template <typename T>
inline void VeResourceManager::addRef(const std::string& resource_id) {
	auto type_idx = typeid(T).hash_code();
	auto& type_refs = m_ref_counts[type_idx];
	auto it = type_refs.find(resource_id);
	if (it != type_refs.end())
		it->second++;
}

template <typename T>
inline void VeResourceManager::release(const std::string& resource_id) {
	auto type_idx = typeid(T).hash_code();
	auto& type_refs = m_ref_counts[type_idx];
	auto it = type_refs.find(resource_id);
	if (it == type_refs.end())
		return;
	if (it->second <= 0)
		return; // already queued for retirement; ignore extra release calls
	if (--it->second == 0) {
		// Record this as the authoritative retire frame for the id. Any earlier
		// entry already in m_pending_unloads for the same id will be silently
		// skipped by processPendingEntry when its retire_frame doesn't match.
		uint64_t retire = m_current_frame + MAX_FRAMES_IN_FLIGHT;
		m_latest_retire_frame[type_idx][resource_id] = retire;
		m_pending_unloads.push_back(PendingUnload{
			retire,
			type_idx,
			resource_id,
		});
	}
}

} // namespace ve
