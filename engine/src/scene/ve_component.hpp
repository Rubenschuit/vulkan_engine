/* Component system for VeGameObject entities.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

// Forward declarations
class VeGameObject;

// ---------------------------------------------------------------------------
// Component type ID system
// ---------------------------------------------------------------------------

// Assigns a unique ID to each different component type.
// Each template instantiation gets its own static local, therefore each 
// component type has its own unique ID.
class VENGINE_API ComponentTypeIDSystem {
public:
	template <typename T>
	static size_t getTypeID() {
		static size_t type_id = m_next_type_id++; // Each template instantiation gets its own static local
		return type_id;
	}

private:
	static size_t m_next_type_id;
};

// ---------------------------------------------------------------------------
// Component base class
// ---------------------------------------------------------------------------
class VENGINE_API Component {
public:
	virtual ~Component() = default;

	virtual void initialize() {} // unused
	virtual void update(float /*delta_time*/) {}
	virtual void render() {} // unused

	void setOwner(VeGameObject* entity) { m_owner = entity; }
	VeGameObject* getOwner() const { return m_owner; }

	template <typename T>
	static size_t getTypeID() {
		return ComponentTypeIDSystem::getTypeID<T>();
	}

protected:
	VeGameObject* m_owner = nullptr;

};

// ---------------------------------------------------------------------------
// TransformComponent
// ---------------------------------------------------------------------------
class VENGINE_API TransformComponent : public Component {
public:
	glm::vec3 translation{0.0f};
	glm::vec3 rotation{0.0f, 0.0f, 0.0f}; // Euler angles in radians
	glm::vec3 scale{1.0f};

	const glm::mat4& getTransform() const;  // local transform
	const glm::mat3& getNormalTransform() const;

private:
	void updateMatrices() const;

	mutable glm::mat4 m_cached_transform{1.0f};
	mutable glm::mat3 m_cached_normal_transform{1.0f};
	mutable glm::vec3 m_last_translation{std::numeric_limits<float>::infinity()};
	mutable glm::vec3 m_last_rotation{std::numeric_limits<float>::infinity()};
	mutable glm::vec3 m_last_scale{std::numeric_limits<float>::infinity()};
};

// ---------------------------------------------------------------------------
// PointLightComponent
// ---------------------------------------------------------------------------
class VENGINE_API PointLightComponent : public Component {
public:
	float intensity{1.0f};
	glm::vec3 color{1.0f};
	bool rotates{true};
	bool casts_shadow{true};

	void update(float delta_time) override;
};

// ---------------------------------------------------------------------------
// MeshComponent
// ---------------------------------------------------------------------------
class VENGINE_API MeshComponent : public Component {
public:
	MeshComponent(ResourceHandle<VeMesh> mesh_h, ResourceHandle<VeMaterial> material_h)
		: mesh_handle(std::move(mesh_h)), material_handle(std::move(material_h)) {}

	void render() override;

	VeMesh* getMesh() const { return mesh_handle.get(); }
	VeMaterial* getMaterial() const { return material_handle.get(); }
	ResourceHandle<VeMesh> getMeshHandle() const { return mesh_handle; }
	ResourceHandle<VeMaterial> getMaterialHandle() const { return material_handle; }
	bool hasMesh() const { return mesh_handle.isValid(); }
	bool hasMaterial() const { return material_handle.isValid(); }

	// Returns mesh local AABB transformed by owner's model matrix. Cached until transform changes.
	VeMesh::AABB getWorldAABB() const;

	float has_texture{0.0f};
	bool has_shadow{true};

private:
	ResourceHandle<VeMesh> mesh_handle;
	ResourceHandle<VeMaterial> material_handle;
	mutable VeMesh::AABB m_cached_world_aabb;
	mutable glm::mat4 m_last_model_matrix{1.0f};
	mutable bool m_world_aabb_valid = false;
};

// TODO: Add more components as needed (camera, animation, etc.)

// suppress implicit instantiation
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<TransformComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<PointLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MeshComponent>();

} // namespace ve
