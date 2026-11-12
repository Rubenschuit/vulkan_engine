#include "pch.hpp"
#include "ui/panels/viewport_panel.hpp"
#include "ui/editor_icons.hpp"
#include "ui/entity_actions.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "scene/camera_view.hpp"
#include "scene/fly_camera_controller.hpp"
#include "scene/scene_manager.hpp"
#include "events/event_bus.hpp"
#include "utils/ve_path.hpp"
#include "utils/ve_ray.hpp"
#include "physics/physics_system.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/orthonormalize.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace ve {

namespace {

constexpr float MIN_CAMERA_SPEED = 0.05f;
constexpr float MAX_CAMERA_SPEED = 1000.0f;
constexpr float MAX_SPRINT_MULTIPLIER = 50.0f;
constexpr float MIN_MOUSE_SENSITIVITY = 0.01f;
constexpr float MAX_MOUSE_SENSITIVITY = 1.0f;
constexpr float CAMERA_SPEED_STEP = 1.25f;   // per wheel notch while looking
constexpr float LIGHT_SURFACE_OFFSET = 0.5f; // metres off the clicked surface
constexpr double SPEED_READOUT_SECONDS = 1.0;
constexpr const char* CONTEXT_MENU_ID = "ViewportContextMenu";

// Where something dropped or created along `ray` lands: the surface it hits, else the z = 0
// ground plane, else 10 m ahead. `normal` faces back toward the ray origin.
glm::vec3 placementPoint(const Ray& ray, Registry& registry, glm::vec3& normal) {
	RayHit hit;
	if (raycastScene(ray, registry, hit)) {
		normal = glm::dot(hit.normal, ray.direction) > 0.0f ? -hit.normal : hit.normal;
		if (glm::dot(normal, normal) < 0.25f)
			normal = -ray.direction;
		return hit.point;
	}
	if (std::abs(ray.direction.z) > 1e-4f) {
		float t = -ray.origin.z / ray.direction.z;
		if (t > 0.0f) {
			normal = glm::vec3(0.0f, 0.0f, 1.0f);
			return ray.origin + t * ray.direction;
		}
	}
	normal = -ray.direction;
	return ray.origin + ray.direction * 10.0f;
}

void formatSpeed(char* buf, size_t size, float speed) {
	snprintf(buf, size, speed < 1.0f ? "%.2f m/s" : (speed < 10.0f ? "%.1f m/s" : "%.0f m/s"), static_cast<double>(speed));
}

} // namespace

void ViewportPanel::setFlyCamera(FlyCameraController* camera) {
	m_fly_camera = camera;
	static constexpr const char* TYPE_NAME = "EditorCamera";
	if (ImGuiSettingsHandler* existing = ImGui::FindSettingsHandler(TYPE_NAME)) {
		existing->UserData = camera;
		return;
	}
	ImGuiSettingsHandler handler;
	handler.TypeName = TYPE_NAME;
	handler.TypeHash = ImHashStr(TYPE_NAME);
	handler.UserData = camera;
	handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* h, const char* name) -> void* {
		return std::strcmp(name, "Settings") == 0 ? h->UserData : nullptr;
	};
	handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
		auto* cam = static_cast<FlyCameraController*>(entry);
		const char* eq = std::strchr(line, '=');
		if (!cam || !eq)
			return;
		char* end = nullptr;
		const float v = std::strtof(eq + 1, &end);
		if (end == eq + 1 || !std::isfinite(v))
			return;
		std::string_view key(line, static_cast<size_t>(eq - line));
		if (key == "MoveSpeed")
			cam->setMoveSpeed(std::clamp(v, MIN_CAMERA_SPEED, MAX_CAMERA_SPEED));
		else if (key == "SprintMultiplier")
			cam->setSprintMultiplier(std::clamp(v, 1.0f, MAX_SPRINT_MULTIPLIER));
		else if (key == "MouseSensitivity")
			cam->setMouseSensitivity(std::clamp(v, MIN_MOUSE_SENSITIVITY, MAX_MOUSE_SENSITIVITY));
	};
	handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
		const auto* cam = static_cast<const FlyCameraController*>(h->UserData);
		if (!cam)
			return;
		buf->appendf("[%s][Settings]\n", h->TypeName);
		buf->appendf("MoveSpeed=%g\n", static_cast<double>(cam->moveSpeed()));
		buf->appendf("SprintMultiplier=%g\n", static_cast<double>(cam->sprintMultiplier()));
		buf->appendf("MouseSensitivity=%g\n", static_cast<double>(cam->mouseSensitivity()));
		buf->append("\n");
	};
	ImGui::AddSettingsHandler(&handler);
}

void ViewportPanel::render(Registry* registry, EditorState& state, UIContext& /*context*/) {
	invalidateImageRect();

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	bool open = ImGui::Begin("Viewport", &state.show_viewport, ImGuiWindowFlags_NoFocusOnAppearing);
	ImGui::PopStyleVar();

	if (open) {
		state.viewport_hovered = ImGui::IsWindowHovered();
		state.viewport_focused = ImGui::IsWindowFocused();

		renderGizmoToolbar(state);
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		renderCameraSelector(registry, state);
		renderCameraSettings();

		ImVec2 size = ImGui::GetContentRegionAvail();
		float dpi_scale = ImGui::GetIO().DisplayFramebufferScale.x;
		state.viewport_width = std::max(0.0f, size.x * dpi_scale);
		state.viewport_height = std::max(0.0f, size.y * dpi_scale);

		// Capture image top-left for gizmo rect (after toolbar, before Image call)
		ImVec2 image_pos = ImGui::GetCursorScreenPos();
		if (size.x > 0.f && size.y > 0.f) {
			m_image_min = image_pos;
			m_image_max = ImVec2(image_pos.x + size.x, image_pos.y + size.y);
		}

		bool drew_image = m_texture_id != VK_NULL_HANDLE && size.x > 0 && size.y > 0;
		if (drew_image)
			ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(m_texture_id)), size);
		else
			ImGui::Text("No viewport image");

		// Drop a model dragged from the Asset Browser at the cursor's surface point.
		if (drew_image && ImGui::BeginDragDropTarget()) {
			const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_GLTF_PATH");
			Ray ray;
			if (payload && payload->Data && m_event_bus && registry && rayUnderMouse(ray)) {
				glm::vec3 normal{0.0f};
				glm::vec3 placement = placementPoint(ray, *registry, normal);

				m_event_bus->emitImmediate(AddModelRequestedEvent{
					.gltf_path = utf8ToPath(static_cast<const char*>(payload->Data)),
					.translation = placement,
					.flip_tex_coord_v = state.import_flip_v});
			}
			ImGui::EndDragDropTarget();
		}

		// Gizmo overlay (drawn on top of the image)
		renderGizmo(registry, state, image_pos.x, image_pos.y, size.x, size.y);

		// Collision shape debug overlay
		if (state.show_collision_shape)
			renderCollisionShape(registry, state, image_pos.x, image_pos.y, size.x, size.y);

		// Raycast picking (left-click in viewport selects entity)
		if (state.viewport_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
		    && !state.gizmo_active && !ImGuizmo::IsOver() && m_camera_view && registry) {
			ImVec2 mouse = ImGui::GetMousePos();
			float uv_x = (mouse.x - image_pos.x) / size.x;
			float uv_y = (mouse.y - image_pos.y) / size.y;

			if (uv_x >= 0.0f && uv_x <= 1.0f && uv_y >= 0.0f && uv_y <= 1.0f) {
				glm::mat4 inv_vp = glm::inverse(m_camera_view->proj * m_camera_view->view);
				Ray ray = screenToWorldRay(uv_x, uv_y, inv_vp);
				RayHit hit;
				bool additive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
				if (raycastScene(ray, *registry, hit)) {
					if (additive)
						state.toggleSelection(hit.entity);
					else
						state.selectSingle(hit.entity);
				} else if (!additive && !state.selected_entities.empty()) {
					state.clearSelection();
				}
			}
		}

		handleRightMouse(registry, state);
		renderContextMenu(registry, state);

		if (state.mouse_look_active && m_fly_camera && state.viewport_camera.isNull()) {
			float wheel = ImGui::GetIO().MouseWheel;
			if (wheel != 0.0f) {
				m_fly_camera->setMoveSpeed(std::clamp(m_fly_camera->moveSpeed() * std::pow(CAMERA_SPEED_STEP, wheel),
					MIN_CAMERA_SPEED, MAX_CAMERA_SPEED));
				m_speed_readout_until = ImGui::GetTime() + SPEED_READOUT_SECONDS;
				ImGui::MarkIniSettingsDirty();
			}
		}
		if (drew_image)
			renderSpeedReadout();

		// Keyboard shortcuts for gizmo mode (when viewport is hovered)
		if (state.viewport_hovered || state.viewport_focused) {
			if (ImGui::IsKeyPressed(ImGuiKey_T) && !ImGui::GetIO().WantTextInput)
				state.gizmo_operation = GizmoOperation::Translate;
			if (ImGui::IsKeyPressed(ImGuiKey_R) && !ImGui::GetIO().WantTextInput)
				state.gizmo_operation = GizmoOperation::Rotate;
			if (ImGui::IsKeyPressed(ImGuiKey_E) && !ImGui::GetIO().WantTextInput)
				state.gizmo_operation = GizmoOperation::Scale;
		}
	}
	ImGui::End();
}

void ViewportPanel::renderGizmoToolbar(EditorState& state) {
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));

	const ImVec4 active_color = ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered];

	// Toolbar button that highlights when `active` and carries a tooltip.
	auto toggleButton = [&](const char* label, bool active, const char* tooltip) {
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, active_color);
		bool clicked = ImGui::Button(label);
		if (active)
			ImGui::PopStyleColor();
		ImGui::SetItemTooltip("%s", tooltip);
		return clicked;
	};

	if (toggleButton("Translate", state.gizmo_operation == GizmoOperation::Translate, "Translate (T)"))
		state.gizmo_operation = GizmoOperation::Translate;
	ImGui::SameLine();
	if (toggleButton("Rotate", state.gizmo_operation == GizmoOperation::Rotate, "Rotate (R)"))
		state.gizmo_operation = GizmoOperation::Rotate;
	ImGui::SameLine();
	if (toggleButton("Scale", state.gizmo_operation == GizmoOperation::Scale, "Scale (E)"))
		state.gizmo_operation = GizmoOperation::Scale;

	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	bool is_world = state.gizmo_space == GizmoSpace::World;
	if (ImGui::Button(is_world ? "World" : "Local"))
		state.gizmo_space = is_world ? GizmoSpace::Local : GizmoSpace::World;
	ImGui::SetItemTooltip("Transform space: %s (click to toggle)", is_world ? "World" : "Local");

	ImGui::SameLine();
	if (toggleButton("Snap", state.gizmo_snap_enabled, "Grid snapping (click to toggle)"))
		state.gizmo_snap_enabled = !state.gizmo_snap_enabled;

	ImGui::PopStyleVar();
}

void ViewportPanel::renderCameraSelector(Registry* registry, EditorState& state) {
	bool selection_is_editor = !isSceneCamera(registry, state.viewport_camera);
	if (selection_is_editor)
		state.viewport_camera = Entity::null();

	std::string current_label;
	if (selection_is_editor) {
		current_label = "Editor Camera";
	} else {
		current_label = registry->getName(state.viewport_camera);
		if (current_label.empty())
			current_label = "<unnamed>";
	}

	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::BeginCombo("##ViewportCamera", current_label.c_str())) {
		if (ImGui::Selectable("Editor Camera", selection_is_editor))
			state.viewport_camera = Entity::null();

		if (registry) {
			auto& pool = registry->cameras();
			for (uint32_t i = 0; i < pool.size(); i++) {
				uint32_t entity_idx = pool.entityIndexData()[i];
				Entity e = registry->entityFromIndex(entity_idx);
				if (!registry->isAlive(e))
					continue;
				const std::string& name = registry->getName(e);
				std::string label = name.empty() ? std::string{"<unnamed>"} : name;
				bool selected = (e == state.viewport_camera);
				ImGui::PushID(static_cast<int>(e.id()));
				if (ImGui::Selectable(label.c_str(), selected))
					state.viewport_camera = e;
				ImGui::PopID();
			}
		}
		ImGui::EndCombo();
	}
}

void ViewportPanel::renderCameraSettings() {
	if (!m_fly_camera)
		return;
	ImGui::SameLine();
	char speed_text[24];
	formatSpeed(speed_text, sizeof(speed_text), m_fly_camera->moveSpeed());
	char label[48];
	snprintf(label, sizeof(label), "%s###CameraSettings", speed_text);
	if (ImGui::Button(label))
		ImGui::OpenPopup("CameraSettings");
	ImGui::SetItemTooltip("Editor camera speed and mouse look");

	if (!ImGui::BeginPopup("CameraSettings"))
		return;
	bool changed = false;
	float speed = m_fly_camera->moveSpeed();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
	if (ImGui::SliderFloat("Move Speed", &speed, MIN_CAMERA_SPEED, MAX_CAMERA_SPEED, "%.2f m/s", ImGuiSliderFlags_Logarithmic)) {
		m_fly_camera->setMoveSpeed(speed);
		changed = true;
	}
	float sprint = m_fly_camera->sprintMultiplier();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
	if (ImGui::SliderFloat("Sprint Multiplier", &sprint, 1.0f, MAX_SPRINT_MULTIPLIER, "%.1fx", ImGuiSliderFlags_Logarithmic)) {
		m_fly_camera->setSprintMultiplier(sprint);
		changed = true;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Speed factor while Shift is held.");
	float sensitivity = m_fly_camera->mouseSensitivity();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
	if (ImGui::SliderFloat("Mouse Sensitivity", &sensitivity, MIN_MOUSE_SENSITIVITY, MAX_MOUSE_SENSITIVITY, "%.3f deg/px",
	                       ImGuiSliderFlags_Logarithmic)) {
		m_fly_camera->setMouseSensitivity(sensitivity);
		changed = true;
	}
	if (ImGui::Button("Reset to Defaults")) {
		m_fly_camera->setMoveSpeed(FlyCameraController::DEFAULT_MOVE_SPEED);
		m_fly_camera->setSprintMultiplier(FlyCameraController::DEFAULT_SPRINT_MULTIPLIER);
		m_fly_camera->setMouseSensitivity(FlyCameraController::DEFAULT_MOUSE_SENSITIVITY);
		changed = true;
	}
	ImGui::TextDisabled("Hold the right mouse button in the viewport to look around;\nscroll while holding it to change the speed.");
	if (changed)
		ImGui::MarkIniSettingsDirty();
	ImGui::EndPopup();
}

bool ViewportPanel::rayUnderMouse(Ray& ray) const {
	if (!m_camera_view || !isImageValid())
		return false;
	ImVec2 mouse = ImGui::GetMousePos();
	float uv_x = (mouse.x - m_image_min.x) / (m_image_max.x - m_image_min.x);
	float uv_y = (mouse.y - m_image_min.y) / (m_image_max.y - m_image_min.y);
	if (uv_x < 0.0f || uv_x > 1.0f || uv_y < 0.0f || uv_y > 1.0f)
		return false;
	ray = screenToWorldRay(uv_x, uv_y, glm::inverse(m_camera_view->proj * m_camera_view->view));
	return true;
}

// Right button on the image: released in place it opens the context menu, dragged past the
// threshold it becomes mouse look. The cursor stays free until then, so a click never hides it.
void ViewportPanel::handleRightMouse(Registry* registry, EditorState& state) {
	if (state.mouse_look_active) {
		m_right_pending = false;
		return;
	}
	Ray ray;
	if (state.viewport_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !state.gizmo_active && rayUnderMouse(ray))
		m_right_pending = true;
	if (!m_right_pending)
		return;

	if (ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right)) {
		m_right_pending = false;
		state.mouse_look_requested = true;
	} else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
		m_right_pending = false;
		if (registry && rayUnderMouse(ray)) {
			m_menu_point = placementPoint(ray, *registry, m_menu_normal);
			ImGui::OpenPopup(CONTEXT_MENU_ID);
		}
	} else if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
		m_right_pending = false;
	}
}

void ViewportPanel::renderContextMenu(Registry* registry, EditorState& state) {
	if (!ImGui::BeginPopup(CONTEXT_MENU_ID))
		return;
	// Acts on the selection, wherever the click was; with nothing selected it creates at the click
	const Entity primary = state.selectedEntity();
	if (registry && !primary.isNull() && registry->isAlive(primary)) {
		const size_t count = state.selected_entities.size();
		if (count == 1) {
			const std::string& name = registry->getName(primary);
			ImGui::SeparatorText(name.empty() ? "<unnamed>" : name.c_str());
		} else {
			char title[32];
			snprintf(title, sizeof(title), "%zu Entities", count);
			ImGui::SeparatorText(title);
		}
		entityContextMenuItems(*registry, state, primary);
		ImGui::Separator();
		if (ImGui::MenuItem("Clear Selection", "Esc"))
			state.clearSelection();
	} else {
		ImGui::SeparatorText("Create Here");
		const glm::vec3 light_position = m_menu_point + m_menu_normal * LIGHT_SURFACE_OFFSET;
		auto create = [&](EntityAction action, glm::vec3 position) {
			state.entity_requests.push_back({.action = action, .position = position});
		};
		if (ImGui::MenuItem(ICON_GROUP "  Empty Entity"))
			create(EntityAction::CreateEmpty, m_menu_point);
		if (ImGui::MenuItem(ICON_POINT_LIGHT "  Point Light"))
			create(EntityAction::CreatePointLight, light_position);
		if (ImGui::MenuItem(ICON_SPOT_LIGHT "  Spot Light"))
			create(EntityAction::CreateSpotLight, light_position);
	}
	ImGui::EndPopup();
}

void ViewportPanel::renderSpeedReadout() {
	if (!m_fly_camera || ImGui::GetTime() >= m_speed_readout_until)
		return;
	char text[32];
	formatSpeed(text, sizeof(text), m_fly_camera->moveSpeed());
	const ImVec2 pad(6.0f, 3.0f);
	const ImVec2 pos(m_image_min.x + 8.0f, m_image_min.y + 8.0f);
	const ImVec2 text_size = ImGui::CalcTextSize(text);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(ImVec2(pos.x - pad.x, pos.y - pad.y),
		ImVec2(pos.x + text_size.x + pad.x, pos.y + text_size.y + pad.y), IM_COL32(0, 0, 0, 170), 4.0f);
	dl->AddText(pos, IM_COL32(255, 255, 255, 255), text);
}

void ViewportPanel::renderGizmo(Registry* registry, EditorState& state, float img_x, float img_y, float img_w, float img_h) {
	state.gizmo_active = false;

	// Unfreeze if gizmo was active last frame but won't be this frame (early return paths)
	auto unfreezeIfNeeded = [&]() {
		if (m_was_gizmo_active && !state.gizmo_active && !m_frozen_entities.empty() && m_physics_system) {
			for (Entity e : m_frozen_entities)
				m_physics_system->unfreezeEntity(e);
			m_frozen_entities.clear();
		}
		m_was_gizmo_active = state.gizmo_active;
	};

	if (!m_camera_view || !registry || state.selectedEntity().isNull()) {
		unfreezeIfNeeded();
		return;
	}
	if (!registry->isAlive(state.selectedEntity())) {
		unfreezeIfNeeded();
		return;
	}

	auto* transform = registry->getComponent<TransformComponent>(state.selectedEntity());
	if (!transform) {
		unfreezeIfNeeded();
		return;
	}

	// Entities the gizmo moves: every selected entity that is alive, transformable,
	// and has no alive selected ancestor
	std::vector<Entity> roots = topMostRoots(*registry, state.selected_entities);
	roots.erase(std::remove_if(roots.begin(), roots.end(),
		[&](Entity e) { return !registry->getComponent<TransformComponent>(e); }), roots.end());

	// ImGuizmo setup
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist();
	ImGuizmo::SetRect(img_x, img_y, img_w, img_h);

	// Map operation enum
	ImGuizmo::OPERATION op;
	switch (state.gizmo_operation) {
		case GizmoOperation::Rotate: op = ImGuizmo::ROTATE; break;
		case GizmoOperation::Scale:  op = ImGuizmo::SCALE; break;
		default:                     op = ImGuizmo::TRANSLATE; break;
	}

	// Scale always uses local space
	ImGuizmo::MODE mode = (op == ImGuizmo::SCALE || state.gizmo_space == GizmoSpace::Local)
		? ImGuizmo::LOCAL : ImGuizmo::WORLD;

	// Snap values
	float snap_values[3] = {0.0f, 0.0f, 0.0f};
	float* snap_ptr = nullptr;
	if (state.gizmo_snap_enabled) {
		float sv = (op == ImGuizmo::ROTATE)    ? state.gizmo_snap_rotate
		         : (op == ImGuizmo::SCALE)     ? state.gizmo_snap_scale
		         :                               state.gizmo_snap_translate;
		snap_values[0] = snap_values[1] = snap_values[2] = sv;
		snap_ptr = snap_values;
	}

	// Get world transform
	const glm::mat4& world = registry->getWorldTransform(state.selectedEntity());
	glm::mat4 model = world;
	const glm::mat4& view = m_camera_view->view;

	glm::vec3 aabb_offset = state.cached_aabb_offset;

	// Apply offset in local space
	glm::vec3 world_offset = glm::vec3(model * glm::vec4(aabb_offset, 1.0f)) - glm::vec3(model[3]);
	model[3] += glm::vec4(world_offset, 0.0f);

	// ImGuizmo expects a standard forward-Z projection
	// Build one from camera parameters since our actual proj is infinite reverse-Z.
	glm::mat4 gizmo_proj = glm::perspective(m_camera_view->fov_y_radians, m_camera_view->aspect,
	                                         m_camera_view->z_near, m_camera_view->z_far);

	auto axisLengths = [](const glm::mat4& m) {
		return glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
	};
	glm::mat4 model_before = model;
	if (!m_was_gizmo_active)
		m_drag_start_axis_lengths = axisLengths(model);
	const glm::mat3 frame_before(model_before);
	if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(gizmo_proj),
	                         op, mode, glm::value_ptr(model), nullptr, snap_ptr)
	    && std::abs(glm::determinant(frame_before)) > 1e-12f) {
		// The gizmo frame changes about its pivot by a world-space linear delta. Every root's origin
		// moves with it, so rotation and scale stay centred on the pivot. Rotation and scale update
		// the TRS components directly; decomposing the local matrix would flip mirrored scale.
		const glm::vec3 pivot_before(model_before[3]);
		const glm::vec3 pivot(model[3]);
		glm::mat3 delta(1.0f);
		glm::vec3 scale_factor(1.0f);
		if (op == ImGuizmo::SCALE) {
			// ImGuizmo scales the frame's axes in place. Each axis keeps a share of its length at drag start
			glm::vec3 lengths = glm::max(axisLengths(model), m_drag_start_axis_lengths * GIZMO_MIN_SCALE_FACTOR);
			scale_factor = lengths / axisLengths(model_before);
			delta = frame_before * glm::mat3(glm::scale(glm::mat4(1.0f), scale_factor)) * glm::inverse(frame_before);
		} else if (op == ImGuizmo::ROTATE) {
			delta = glm::mat3(model) * glm::inverse(frame_before);
		}

		for (Entity r : roots) {
			auto* tc = registry->getComponent<TransformComponent>(r);
			Entity parent = registry->getParent(r);
			const glm::mat4 parent_world = parent.isNull() ? glm::mat4(1.0f) : registry->getWorldTransform(parent);
			const glm::vec3 origin(registry->getWorldTransform(r)[3]);
			const glm::vec3 new_origin = pivot + delta * (origin - pivot_before);
			tc->setTranslation(glm::vec3(glm::inverse(parent_world) * glm::vec4(new_origin, 1.0f)));
			if (op == ImGuizmo::ROTATE) {
				// The world rotation expressed in the parent's space; exact unless the parent scales non-uniformly
				const glm::mat3 parent_frame(parent_world);
				glm::quat local_delta = glm::quat_cast(glm::orthonormalize(glm::inverse(parent_frame) * delta * parent_frame));
				tc->setRotation(glm::normalize(local_delta * tc->getRotation()));
			} else if (op == ImGuizmo::SCALE) {
				tc->setScale(tc->getScale() * scale_factor);
			}
		}
	}

	state.gizmo_active = ImGuizmo::IsUsing();

	// Freeze physics when gizmo drag starts, unfreeze when it ends
	if (state.gizmo_active && !m_was_gizmo_active && m_physics_system) {
		for (Entity r : roots) {
			m_physics_system->freezeEntity(r);
			m_frozen_entities.push_back(r);
		}
	}
	unfreezeIfNeeded();
}

// ── Collision shape wireframe overlay ────────────────────────────────────────

static ImVec2 worldToScreen(const glm::vec3& world, const glm::mat4& vp, float img_x, float img_y, float img_w, float img_h) {
	glm::vec4 clip = vp * glm::vec4(world, 1.0f);
	if (clip.w <= 0.0f)
		return {-1.0f, -1.0f};
	glm::vec3 ndc = glm::vec3(clip) / clip.w;
	return {
		img_x + (ndc.x * 0.5f + 0.5f) * img_w,
		img_y + (ndc.y * 0.5f + 0.5f) * img_h
	};
}

static void drawLine3D(ImDrawList* dl, const glm::vec3& a, const glm::vec3& b,
	const glm::mat4& vp, float img_x, float img_y, float img_w, float img_h, ImU32 col) {
	ImVec2 sa = worldToScreen(a, vp, img_x, img_y, img_w, img_h);
	ImVec2 sb = worldToScreen(b, vp, img_x, img_y, img_w, img_h);
	if (sa.x < 0 || sb.x < 0)
		return;
	dl->AddLine(sa, sb, col, 1.5f);
}

static void drawWireBox(ImDrawList* dl, const glm::vec3& center, const glm::quat& rot, const glm::vec3& he,
	const glm::mat4& vp, float ix, float iy, float iw, float ih, ImU32 col) {
	glm::vec3 corners[8];
	for (int i = 0; i < 8; i++) {
		glm::vec3 local{
			(i & 1) ? he.x : -he.x,
			(i & 2) ? he.y : -he.y,
			(i & 4) ? he.z : -he.z
		};
		corners[i] = center + rot * local;
	}
	// 12 edges of a box
	static constexpr int edges[12][2] = {
		{0,1},{2,3},{4,5},{6,7}, // x-axis edges
		{0,2},{1,3},{4,6},{5,7}, // y-axis edges
		{0,4},{1,5},{2,6},{3,7}  // z-axis edges
	};
	for (auto& e : edges)
		drawLine3D(dl, corners[e[0]], corners[e[1]], vp, ix, iy, iw, ih, col);
}

static void drawWireSphere(ImDrawList* dl, const glm::vec3& center, const glm::quat& rot, float radius,
	const glm::mat4& vp, float ix, float iy, float iw, float ih, ImU32 col) {
	constexpr int segments = 32;
	// Draw 3 great circles (XY, XZ, YZ planes)
	glm::vec3 axes[3] = {
		rot * glm::vec3(1, 0, 0),
		rot * glm::vec3(0, 1, 0),
		rot * glm::vec3(0, 0, 1)
	};
	for (int ring = 0; ring < 3; ring++) {
		glm::vec3 u = axes[ring];
		glm::vec3 v = axes[(ring + 1) % 3];
		glm::vec3 prev = center + u * radius;
		for (int i = 1; i <= segments; i++) {
			float angle = glm::two_pi<float>() * static_cast<float>(i) / segments;
			glm::vec3 cur = center + (u * cosf(angle) + v * sinf(angle)) * radius;
			drawLine3D(dl, prev, cur, vp, ix, iy, iw, ih, col);
			prev = cur;
		}
	}
}

static void drawWireCapsule(ImDrawList* dl, const glm::vec3& center, const glm::quat& rot,
	float radius, float half_height, const glm::mat4& vp, float ix, float iy, float iw, float ih, ImU32 col) {
	constexpr int segments = 32;
	glm::vec3 up = rot * glm::vec3(0, 1, 0);
	glm::vec3 right = rot * glm::vec3(1, 0, 0);
	glm::vec3 forward = rot * glm::vec3(0, 0, 1);

	glm::vec3 top = center + up * half_height;
	glm::vec3 bot = center - up * half_height;

	// Cylinder body: 2 circles + 4 vertical lines
	for (int cap = 0; cap < 2; cap++) {
		glm::vec3 c = cap == 0 ? top : bot;
		glm::vec3 prev = c + right * radius;
		for (int i = 1; i <= segments; i++) {
			float angle = glm::two_pi<float>() * static_cast<float>(i) / segments;
			glm::vec3 cur = c + (right * cosf(angle) + forward * sinf(angle)) * radius;
			drawLine3D(dl, prev, cur, vp, ix, iy, iw, ih, col);
			prev = cur;
		}
	}
	// 4 vertical lines
	for (int i = 0; i < 4; i++) {
		float angle = glm::half_pi<float>() * static_cast<float>(i);
		glm::vec3 offset = (right * cosf(angle) + forward * sinf(angle)) * radius;
		drawLine3D(dl, top + offset, bot + offset, vp, ix, iy, iw, ih, col);
	}
	// Hemisphere arcs
	for (int cap = 0; cap < 2; cap++) {
		glm::vec3 c = cap == 0 ? top : bot;
		float sign = cap == 0 ? 1.0f : -1.0f;
		for (int arc = 0; arc < 2; arc++) {
			glm::vec3 tangent = arc == 0 ? right : forward;
			glm::vec3 prev = c + tangent * radius;
			for (int i = 1; i <= segments / 2; i++) {
				float angle = glm::pi<float>() * static_cast<float>(i) / (segments / 2);
				glm::vec3 cur = c + (tangent * cosf(angle) + up * sign * sinf(angle)) * radius;
				drawLine3D(dl, prev, cur, vp, ix, iy, iw, ih, col);
				prev = cur;
			}
		}
	}
}

void ViewportPanel::renderCollisionShape(Registry* registry, EditorState& state,
	float img_x, float img_y, float img_w, float img_h) {
	if (!m_physics_system || !m_camera_view || !registry || state.selectedEntity().isNull())
		return;

	auto shape = m_physics_system->getDebugShape(state.selectedEntity(), *registry);
	if (!shape)
		return;

	glm::mat4 vp = m_camera_view->proj * m_camera_view->view;
	ImDrawList* dl = ImGui::GetWindowDrawList();

	ImU32 col = shape->is_dynamic ? IM_COL32(0, 255, 100, 200) : IM_COL32(0, 150, 255, 200);

	auto drawShape = [&](const DebugShape& s, auto& self) -> void {
		glm::vec3 center = s.position;
		switch (s.type) {
			case DebugShapeType::Box:
				drawWireBox(dl, center, s.rotation, s.extents, vp, img_x, img_y, img_w, img_h, col);
				break;
			case DebugShapeType::Sphere:
				drawWireSphere(dl, center, s.rotation, s.extents.x, vp, img_x, img_y, img_w, img_h, col);
				break;
			case DebugShapeType::Capsule:
				drawWireCapsule(dl, center, s.rotation, s.extents.x, s.extents.y,
					vp, img_x, img_y, img_w, img_h, col);
				break;
			case DebugShapeType::ConvexHull:
				for (const auto& [a, b] : s.hull_edges) {
					glm::vec3 pa = center + s.rotation * s.hull_vertices[a];
					glm::vec3 pb = center + s.rotation * s.hull_vertices[b];
					drawLine3D(dl, pa, pb, vp, img_x, img_y, img_w, img_h, col);
				}
				break;
			case DebugShapeType::Compound:
				for (const auto& child : s.sub_shapes)
					self(child, self);
				break;
		}
	};
	drawShape(*shape, drawShape);
}

} // namespace ve