/* Component system for VeGameObject entities.
 */
#pragma once
#include "ve_export.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

// Forward declaration
class VeGameObject;
class VeModel;

// ---------------------------------------------------------------------------
// Component type ID system
// ---------------------------------------------------------------------------
class ComponentTypeIDSystem {
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
// TransformComponent - position, rotation (Euler), scale
// ---------------------------------------------------------------------------
class VENGINE_API TransformComponent : public Component {
public:
	glm::vec3 translation{0.0f};
	glm::vec3 rotation{0.0f, 0.0f, 0.0f}; // Euler angles in radians
	glm::vec3 scale{1.0f};

	const glm::mat4& getTransform() const;
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
// PointLightComponent - point light properties
// ---------------------------------------------------------------------------
class VENGINE_API PointLightComponent : public Component {
public:
	float intensity{1.0f};
	bool rotates{true};
	bool casts_shadow{true};
};

// ---------------------------------------------------------------------------
// ModelComponent - 3D mesh reference
// ---------------------------------------------------------------------------
class VENGINE_API ModelComponent : public Component {
public:
	explicit ModelComponent(std::shared_ptr<VeModel> model) : model(std::move(model)) {}

	std::shared_ptr<VeModel> model;

	bool hasModel() const { return model != nullptr; }
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

} // namespace ve
