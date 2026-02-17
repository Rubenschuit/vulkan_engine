/* Resource manager and handle for caching and lifetime management of resources
 * such as models and textures. TODO: add support for other resource types suchh as audio and shaders.
 * Based on:
 * https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Engine_Architecture/04_resource_management.html
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource.hpp"
#include "vulkan/ve_device.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material_properties.hpp"

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
	explicit VeResourceManager(VeDevice& device);

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

	// Create VeMaterial from texture paths. pool and layout can be null for untextured materials.
	ResourceHandle<VeMaterial> createMaterial(const std::string& resource_id,
	                                         const std::filesystem::path& albedo_path,
	                                         const std::filesystem::path& normal_path,
	                                         const std::filesystem::path& metallic_roughness_path,
	                                         const std::filesystem::path& occlusion_path,
	                                         const std::filesystem::path& emissive_path,
	                                         MaterialAlphaProps alpha_props,
	                                         MaterialFactors factors,
	                                         class VeDescriptorPool* pool = nullptr,
	                                         class VeDescriptorSetLayout* layout = nullptr,
	                                         bool flip_tex_coord_v = false);

	// Forcibly unload all resources regardless of reference count.
	void unloadAll();

	VeDevice& getDevice() { return m_device; }
	const VeDevice& getDevice() const { return m_device; }

private:
	VeDevice& m_device;
	// Map of resource type to map of resource id to resource.
	// Used to store all resources.
	std::unordered_map<size_t, std::unordered_map<std::string, std::shared_ptr<Resource>>> m_resources;
	// Map of resource type to map of resource id to reference count.
	// Used to track the number of references to each resource.
	std::unordered_map<size_t, std::unordered_map<std::string, int>> m_ref_counts;

	template <typename T>
	friend class ResourceHandle;
};

} // namespace ve
