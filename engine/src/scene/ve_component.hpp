/* Component system for VeGameObject entities.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

// Forward declarations
class VeGameObject;
class VeModel;  // in resources/ve_model.hpp

// ---------------------------------------------------------------------------
// Component type ID system
// ---------------------------------------------------------------------------
class VENGINE_API ComponentTypeIDSystem {
public:
	template <typename T>
	static size_t getTypeID() {
		static size_t type_id = m_next_type_id++;
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

	virtual void initialize() {}
	virtual void update(float /*delta_time*/) {}

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
	bool rotates{true};
	bool casts_shadow{true};
};

// ---------------------------------------------------------------------------
// MeshComponent
// ---------------------------------------------------------------------------
class VENGINE_API MeshComponent : public Component {
public:
	explicit MeshComponent(ResourceHandle<VeMesh> handle, uint32_t material_index = 0)
		: mesh_handle(std::move(handle)), m_material_index(material_index) {}

	VeMesh* getMesh() const { return mesh_handle.get(); }
	ResourceHandle<VeMesh> getMeshHandle() const { return mesh_handle; }
	bool hasMesh() const { return mesh_handle.isValid(); }
	uint32_t getMaterialIndex() const { return m_material_index; }

	void setModel(VeModel* model) { m_model = model; }
	VeModel* getModel() const { return m_model; }

private:
	ResourceHandle<VeMesh> mesh_handle;
	uint32_t m_material_index;
	VeModel* m_model = nullptr;
};

// ---------------------------------------------------------------------------
// MaterialComponent - color, texture flag, shadow flag
// ---------------------------------------------------------------------------
class VENGINE_API MaterialComponent : public Component {
public:
	glm::vec3 color{1.0f};
	float has_texture{0.0f};
	bool has_shadow{true};
};

// TODO: Add more components as needed (camera, animation, etc.)

// Explicit instantiations provided by engine; prevents duplicate symbols when tests use these
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<TransformComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<PointLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MeshComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MaterialComponent>();

} // namespace ve
