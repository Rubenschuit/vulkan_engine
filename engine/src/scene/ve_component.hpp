/* Component system for ECS entities. Components are plain data holders that
 * can be attached to entities. Systems operate on entities with specific component combinations.
 */
#pragma once
#include "ve_export.hpp"
#include "ve_entity.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

// Forward declarations
class Registry;

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

	void setContext(Entity entity, Registry* registry) { m_entity = entity; m_registry = registry; }
	Entity getEntity() const { return m_entity; }
	Registry* getRegistry() const { return m_registry; }

	template <typename T>
	static size_t getTypeID() {
		return ComponentTypeIDSystem::getTypeID<T>();
	}

protected:
	Entity m_entity;
	Registry* m_registry = nullptr;

};

// ---------------------------------------------------------------------------
// TransformComponent
// ---------------------------------------------------------------------------
class VENGINE_API TransformComponent : public Component {
public:
	const glm::mat4& getTransform() const;  // local transform
	const glm::mat3& getNormalTransform() const;

	const glm::vec3& getTranslation() const { return translation; }
	const glm::quat& getRotation() const { return rotation; }
	const glm::vec3& getScale() const { return scale; }

	void setTranslation(glm::vec3 pos);
	void setRotation(glm::quat q);
	// Set rotation from Euler angles in radians (order: X, Y, Z applied as Rz*Ry*Rx)
	void setRotationEuler(glm::vec3 euler_rad);
	void setScale(glm::vec3 s);

private:
	void updateTransform() const;

	glm::vec3 translation{0.0f};
	glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // identity
	glm::vec3 scale{1.0f};

	mutable glm::mat4 m_cached_transform{1.0f};
	mutable glm::mat3 m_cached_normal_transform{1.0f};
	mutable bool m_transform_dirty{true};
};

// ---------------------------------------------------------------------------
// PointLightComponent
// ---------------------------------------------------------------------------
class VENGINE_API PointLightComponent : public Component {
public:
	float intensity{1.0f};
	glm::vec3 color{1.0f};
	bool rotates{false};
	bool casts_shadow{false};
	float range{0.0f};

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
	const VeMesh::AABB& getWorldAABB() const;
	void invalidateWorldAABB();

	float has_texture{0.0f};
	bool has_shadow{true};

private:
	void updateWorldAABB() const;

	ResourceHandle<VeMesh> mesh_handle;
	ResourceHandle<VeMaterial> material_handle;
	mutable VeMesh::AABB m_cached_world_aabb;
	mutable bool m_world_aabb_dirty{true};
};

// ---------------------------------------------------------------------------
// DirectionalLightComponent
// ---------------------------------------------------------------------------
enum class CelestialType : uint8_t { Moon = 0, Sun = 1 };

class VENGINE_API DirectionalLightComponent : public Component {
public:
	glm::vec3 direction{0.f, -1.f, -1.f};  // world-space light direction (toward surface)
	glm::vec3 color{1.f};
	float intensity{1.f};
	bool casts_shadow{false};
	CelestialType celestial_type{CelestialType::Sun};
};

// TODO: Add more components as needed (camera, animation, etc.)

// suppress implicit instantiation
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<TransformComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<PointLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<DirectionalLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MeshComponent>();

} // namespace ve
