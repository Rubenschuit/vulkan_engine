/* VeGameObject - Entity with component-based architecture.
 * Based on:
 * https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Engine_Architecture/03_component_systems.html
 *
 * Game objects are entities in the scene that can have one or no component of each type.
 * This class holds a map of component type IDs to component pointers and a vector of components.
 * Use AddComponent<T>() to add components, GetComponent<T>() to retrieve them.
 */
#pragma once
#include "ve_export.hpp"
#include "ve_component.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

class VENGINE_API VeGameObject {
public:
	static VeGameObject createGameObject();
	static VeGameObject createPointLight(float intensity = 1.0f, float radius = 1.0f, glm::vec3 color = glm::vec3(1.0f));

	VeGameObject(const VeGameObject&) = delete;
	VeGameObject& operator=(const VeGameObject&) = delete;
	VeGameObject(VeGameObject&&) = default;
	VeGameObject& operator=(VeGameObject&&) = default;

	uint32_t getId() const { return m_id; }

	template <typename T, typename... Args>
	T* addComponent(Args&&... args) {
		static_assert(std::is_base_of_v<Component, T>, "T must derive from Component");

		const size_t type_id = Component::getTypeID<T>();

		// Check if component of this type already exists, if so return the existing component
		auto it = m_component_map.find(type_id);
		if (it != m_component_map.end()) {
			return static_cast<T*>(it->second);
		}

		auto component = std::make_unique<T>(std::forward<Args>(args)...);
		T* component_ptr = component.get();
		component_ptr->setOwner(this);
		m_component_map[type_id] = component_ptr;
		m_components.push_back(std::move(component));
		return component_ptr;
	}

	template <typename T>
	T* getComponent() {
		const size_t type_id = Component::getTypeID<T>();
		auto it = m_component_map.find(type_id);
		if (it != m_component_map.end()) {
			return static_cast<T*>(it->second);
		}
		return nullptr;
	}

	template <typename T>
	const T* getComponent() const {
		const size_t type_id = Component::getTypeID<T>();
		auto it = m_component_map.find(type_id);
		if (it != m_component_map.end()) {
			return static_cast<const T*>(it->second);
		}
		return nullptr;
	}

	template <typename T>
	bool removeComponent() {
		const size_t type_id = Component::getTypeID<T>();
		auto it = m_component_map.find(type_id);
		if (it != m_component_map.end()) {
			Component* component_ptr = it->second;
			m_component_map.erase(it);

			for (auto comp_it = m_components.begin(); comp_it != m_components.end(); ++comp_it) {
				if (comp_it->get() == component_ptr) {
					m_components.erase(comp_it);
					return true;
				}
			}
		}
		return false;
	}

	// Convenience functions:
	const glm::mat4& getTransform() const;
	const glm::mat3& getNormalTransform() const;

private:
	explicit VeGameObject(uint32_t id) : m_id(id) {}

	uint32_t m_id;
	std::vector<std::unique_ptr<Component>> m_components;
	std::unordered_map<size_t, Component*> m_component_map;
};

} // namespace ve
