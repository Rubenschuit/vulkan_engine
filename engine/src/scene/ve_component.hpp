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
	float getIntensity() const { return m_intensity; }
	const glm::vec3& getColor() const { return m_color; }
	float getRange() const { return m_range; }
	bool getRotates() const { return m_rotates; }
	bool getCastsShadow() const { return m_casts_shadow; }

	void setIntensity(float v);
	void setColor(const glm::vec3& v);
	void setRange(float v);
	void setRotates(bool v);
	void setCastsShadow(bool v);

	/// Returns range if explicitly set, otherwise derives from intensity/color
	/// using the KHR_lights_punctual cutoff threshold. Cached until dirty.
	float getEffectiveRange() const;

	void update(float delta_time) override;

private:
	void updateEffectiveRange() const;

	float m_intensity{1.0f};
	glm::vec3 m_color{1.0f};
	float m_range{0.0f};
	bool m_rotates{false};
	bool m_casts_shadow{false};

	mutable float m_effective_range{0.0f};
	mutable bool m_range_dirty{true};
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
	mutable uint32_t cached_lod{0};

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
	const glm::vec3& getDirection() const { return m_direction; }
	const glm::vec3& getColor() const { return m_color; }
	float getIntensity() const { return m_intensity; }
	bool getCastsShadow() const { return m_casts_shadow; }
	CelestialType getCelestialType() const { return m_celestial_type; }

	void setDirection(const glm::vec3& v);
	void setColor(const glm::vec3& v);
	void setIntensity(float v);
	void setCastsShadow(bool v);
	void setCelestialType(CelestialType t);

private:
	glm::vec3 m_direction{0.f, -1.f, -1.f};  // world-space light direction (toward surface)
	glm::vec3 m_color{1.f};
	float m_intensity{1.f};
	bool m_casts_shadow{false};
	CelestialType m_celestial_type{CelestialType::Sun};
};

// ---------------------------------------------------------------------------
// SpotLightComponent
// ---------------------------------------------------------------------------
class VENGINE_API SpotLightComponent : public Component {
public:
	float getIntensity() const { return m_intensity; }
	const glm::vec3& getColor() const { return m_color; }
	float getRange() const { return m_range; }
	const glm::vec3& getDirection() const { return m_direction; }
	float getInnerConeAngle() const { return m_inner_cone_angle; }
	float getOuterConeAngle() const { return m_outer_cone_angle; }
	bool getCastsShadow() const { return m_casts_shadow; }

	void setIntensity(float v);
	void setColor(const glm::vec3& v);
	void setRange(float v);
	void setDirection(const glm::vec3& v);
	void setInnerConeAngle(float radians);
	void setOuterConeAngle(float radians);
	void setCastsShadow(bool v);

	/// Returns range if explicitly set, otherwise derives from intensity
	float getEffectiveRange() const;

private:
	void updateEffectiveRange() const;

	float m_intensity{1.0f};
	glm::vec3 m_color{1.0f};
	float m_range{0.0f};// 0 means derive from intensity
	glm::vec3 m_direction{0.f, 0.f, -1.f}; // local-space
	float m_inner_cone_angle{glm::radians(25.0f)};        
	float m_outer_cone_angle{glm::radians(35.0f)};        
	bool m_casts_shadow{false};

	mutable float m_effective_range{0.0f};
	mutable bool m_range_dirty{true};
};

// ---------------------------------------------------------------------------
// RigidbodyComponent
// ---------------------------------------------------------------------------
enum class PhysicsShapeType : uint8_t {
	Box,
	Sphere,
	Capsule,
	ConvexHull,
	MeshStatic // triangle mesh from VeMesh CPU data
};

enum class PhysicsMotionType : uint8_t {
	Static,
	Kinematic,
	Dynamic
};

struct PhysicsShapeDesc {
	PhysicsShapeType type = PhysicsShapeType::Box;
};

class VENGINE_API RigidbodyComponent : public Component {
public:
	RigidbodyComponent() = default;

	RigidbodyComponent(const RigidbodyComponent& other)
		: Component(other)
		, m_motion_type(other.m_motion_type)
		, m_shape(other.m_shape)
		, m_mass(other.m_mass)
		, m_friction(other.m_friction)
		, m_restitution(other.m_restitution)
		, m_hull_tolerance(other.m_hull_tolerance)
		, m_body_id(UINT32_MAX)
		, m_dirty(false) {}

	RigidbodyComponent& operator=(const RigidbodyComponent& other) {
		if (this != &other) {
			Component::operator=(other);
			m_motion_type = other.m_motion_type;
			m_shape = other.m_shape;
			m_mass = other.m_mass;
			m_friction = other.m_friction;
			m_restitution = other.m_restitution;
			m_hull_tolerance = other.m_hull_tolerance;
		}
		return *this;
	}

	PhysicsMotionType getMotionType() const { return m_motion_type; }
	void setMotionType(PhysicsMotionType t);

	const PhysicsShapeDesc& getShapeDesc() const { return m_shape; }
	void setShapeDesc(const PhysicsShapeDesc& s);

	float getMass() const { return m_mass; }
	void setMass(float m);

	float getFriction() const { return m_friction; }
	void setFriction(float f);

	float getRestitution() const { return m_restitution; }
	void setRestitution(float r);

	float getHullTolerance() const { return m_hull_tolerance; }
	void setHullTolerance(float t);

	uint32_t getBodyId() const { return m_body_id; }
	void setBodyId(uint32_t id) { m_body_id = id; }
	bool hasBody() const { return m_body_id != UINT32_MAX; }

	bool isDirty() const { return m_dirty; }
	void clearDirty() { m_dirty = false; }

private:
	PhysicsMotionType m_motion_type = PhysicsMotionType::Static;
	PhysicsShapeDesc m_shape;
	float m_mass = 1.0f;
	float m_friction = 0.5f;
	float m_restitution = 0.3f;
	float m_hull_tolerance = 0.05f;
	uint32_t m_body_id = UINT32_MAX;
	bool m_dirty = false;
};

// suppress implicit instantiation
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<TransformComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<PointLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<DirectionalLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MeshComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<SpotLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<RigidbodyComponent>();

} // namespace ve
