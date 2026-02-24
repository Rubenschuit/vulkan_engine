#pragma once
#include "ve_export.hpp"
#include "scene/ve_entity.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <variant>
#include <optional>

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

using ComponentClipboard = std::variant<CopiedTransform, CopiedPointLight, CopiedDirectionalLight>;

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
	Entity selected_entity = Entity::null();
	bool selection_changed = false;

	// Panel visibility
	bool show_hierarchy = true;
	bool show_inspector = true;
	bool show_viewport = true;
	bool show_performance = true;
	bool show_settings = true;
	bool show_environment = true;

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

	// Selection outline
	float outline_width = 4.0f;
	glm::vec3 outline_color{1.0f, 0.6f, 0.0f};
	bool outline_enabled = true;
};

} // namespace ve
