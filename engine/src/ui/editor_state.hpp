#pragma once
#include "ve_export.hpp"
#include "scene/ve_entity.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <variant>
#include <optional>
#include <vector>
#include <algorithm>

namespace ve {

enum class CelestialType : uint8_t;

// Component clipboard data (snapshots for copy/paste)
struct CopiedTransform {
	glm::vec3 translation{0.0f};
	glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 scale{1.0f};
};

struct CopiedPointLight {
	float intensity{1.0f};
	glm::vec3 color{1.0f};
	float range{0.0f};
	bool rotates{false};
	bool casts_shadow{false};
};

struct CopiedDirectionalLight {
	glm::vec3 direction{0.f, -1.f, -1.f};
	glm::vec3 color{1.f};
	float intensity{1.f};
	bool casts_shadow{false};
	uint8_t celestial_type{1}; // stored as uint8_t
};

struct CopiedSpotLight {
	float intensity{1.0f};
	glm::vec3 color{1.0f};
	float range{0.0f};
	glm::vec3 direction{0.f, 0.f, -1.f};
	float inner_cone_angle{glm::radians(25.0f)};
	float outer_cone_angle{glm::radians(35.0f)};
	bool casts_shadow{false};
};

struct CopiedAreaLight {
	float intensity{1.0f};
	glm::vec3 color{1.0f};
	bool two_sided{false};
	float range{0.0f};
};

struct CopiedRigidbody {
	uint8_t motion_type{0};
	uint8_t shape_type{0};
	glm::vec3 half_extents{0.0f};
	float mass{1.0f};
	float friction{0.5f};
	float restitution{0.3f};
	float hull_tolerance{0.05f};
};

struct CopiedCamera {
	uint8_t projection{0};
	float fov_y_radians{glm::radians(55.0f)};
	float ortho_size{10.0f};
	float near_plane{0.1f};
	float far_plane{1000.0f};
	bool active{true};
	int priority{0};
};

using ComponentClipboard = std::variant<CopiedTransform, CopiedPointLight, CopiedDirectionalLight, CopiedSpotLight, CopiedAreaLight, CopiedRigidbody, CopiedCamera>;

enum class GizmoOperation : int {
	Translate = 0,
	Rotate = 1,
	Scale = 2,
};

enum class GizmoSpace : int {
	World = 0,
	Local = 1,
};

struct VENGINE_API EditorState {
	// Mode
	bool editor_mode = true; // true = docking editor, false = fullscreen

	// Selection
	std::vector<Entity> selected_entities;
	bool selection_changed = false;

	Entity selectedEntity() const {
		return selected_entities.empty() ? Entity::null() : selected_entities.back();
	}
	bool isSelected(Entity e) const {
		return std::find(selected_entities.begin(), selected_entities.end(), e) != selected_entities.end();
	}
	void clearSelection() {
		selected_entities.clear();
		selection_changed = true;
	}
	void selectSingle(Entity e) {
		selected_entities.clear();
		if (!e.isNull())
			selected_entities.push_back(e);
		selection_changed = true;
	}
	void removeFromSelection(Entity e) {
		selected_entities.erase(std::remove(selected_entities.begin(), selected_entities.end(), e), selected_entities.end());
		selection_changed = true;
	}
	void addToSelection(Entity e) {
		if (e.isNull())
			return;
		selected_entities.erase(std::remove(selected_entities.begin(), selected_entities.end(), e), selected_entities.end());
		selected_entities.push_back(e);
		selection_changed = true;
	}
	void toggleSelection(Entity e) {
		if (e.isNull())
			return;
		if (isSelected(e))
			removeFromSelection(e);
		else
			selected_entities.push_back(e);
		selection_changed = true;
	}

	// Camera the viewport renders through. Null = editor camera; otherwise an entity with
	// CameraComponent. The application falls back to the editor camera if the entity becomes
	// invalid.
	Entity viewport_camera = Entity::null();

	// Entity driven by WASD instead of the editor camera. Null = none.
	Entity possessed_entity = Entity::null();

	// Panel visibility
	bool show_hierarchy = true;
	bool show_inspector = true;
	bool show_asset_browser = true;
	bool show_viewport = true;
	bool show_performance = true;
	bool show_settings = true;
	bool show_environment = true;
	bool show_debug = true;
	bool show_app_settings = true;

	// Viewport state (updated by viewport panel each frame)
	bool viewport_hovered = false;
	bool viewport_focused = false;
	float viewport_width = 0.0f;
	float viewport_height = 0.0f;

	// Component clipboard
	std::optional<ComponentClipboard> component_clipboard;

	// Gizmo state
	GizmoOperation gizmo_operation = GizmoOperation::Translate;
	GizmoSpace gizmo_space = GizmoSpace::World;
	bool gizmo_snap_enabled = false;
	float gizmo_snap_translate = 0.5f;
	float gizmo_snap_rotate = 15.0f;
	float gizmo_snap_scale = 0.1f;
	bool gizmo_active = false;
	glm::vec3 cached_aabb_offset{0.0f}; // local-space AABB center for gizmo placement

	// Physics debug
	bool show_collision_shape = false;

	// Windows
	bool show_keybindings = false;

	// Selection outline
	float outline_width = 4.0f;
	glm::vec3 outline_color{1.0f, 0.6f, 0.0f};
	bool outline_enabled = true;

	// Asset import
	bool import_flip_v = false;
};

} // namespace ve
