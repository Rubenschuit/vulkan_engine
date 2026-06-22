#include "pch.hpp"
#include "ui/panels/viewport_panel.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "scene/camera_view.hpp"
#include "utils/ve_ray.hpp"
#include "physics/physics_system.hpp"
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>

namespace ve {

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
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		renderCameraSelector(registry, state);

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

		if (m_texture_id != VK_NULL_HANDLE && size.x > 0 && size.y > 0)
			ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(m_texture_id)), size);
		else
			ImGui::Text("No viewport image");

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
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));

	const ImVec4 active_color = ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered];
	bool is_translate = state.gizmo_operation == GizmoOperation::Translate;
	bool is_rotate = state.gizmo_operation == GizmoOperation::Rotate;
	bool is_scale = state.gizmo_operation == GizmoOperation::Scale;

	if (is_translate) ImGui::PushStyleColor(ImGuiCol_Button, active_color);
	if (ImGui::SmallButton("T Translate"))
		state.gizmo_operation = GizmoOperation::Translate;
	if (is_translate) ImGui::PopStyleColor();

	ImGui::SameLine();
	if (is_rotate) ImGui::PushStyleColor(ImGuiCol_Button, active_color);
	if (ImGui::SmallButton("R Rotate"))
		state.gizmo_operation = GizmoOperation::Rotate;
	if (is_rotate) ImGui::PopStyleColor();

	ImGui::SameLine();
	if (is_scale) ImGui::PushStyleColor(ImGuiCol_Button, active_color);
	if (ImGui::SmallButton("E Scale"))
		state.gizmo_operation = GizmoOperation::Scale;
	if (is_scale) ImGui::PopStyleColor();

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	bool is_world = state.gizmo_space == GizmoSpace::World;
	if (!is_world) ImGui::PushStyleColor(ImGuiCol_Button, active_color);
	if (ImGui::SmallButton(is_world ? "World" : "Local"))
		state.gizmo_space = is_world ? GizmoSpace::Local : GizmoSpace::World;
	if (!is_world) ImGui::PopStyleColor();

	ImGui::SameLine();
	if (state.gizmo_snap_enabled) ImGui::PushStyleColor(ImGuiCol_Button, active_color);
	if (ImGui::SmallButton(state.gizmo_snap_enabled ? "Snap: ON" : "Snap: OFF"))
		state.gizmo_snap_enabled = !state.gizmo_snap_enabled;
	if (state.gizmo_snap_enabled) ImGui::PopStyleColor();

	ImGui::PopStyleVar(2);
}

void ViewportPanel::renderCameraSelector(Registry* registry, EditorState& state) {
	bool selection_is_editor = state.viewport_camera.isNull()
		|| !registry
		|| !registry->isAlive(state.viewport_camera)
		|| !registry->hasComponent<CameraComponent>(state.viewport_camera);
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

void ViewportPanel::renderGizmo(Registry* registry, EditorState& state, float img_x, float img_y, float img_w, float img_h) {
	state.gizmo_active = false;

	// Unfreeze if gizmo was active last frame but won't be this frame (early return paths)
	auto unfreezeIfNeeded = [&]() {
		if (m_was_gizmo_active && !state.gizmo_active && !m_frozen_entities.empty() && m_physics_system) {
			for (Entity e : m_frozen_entities)
				m_physics_system->unfreezeBody(e);
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

	glm::mat4 model_before = model;
	if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(gizmo_proj),
	                         op, mode, glm::value_ptr(model), nullptr, snap_ptr)) {
		if (roots.size() == 1 && roots[0] == state.selectedEntity()) {
			// Single selection
			// Remove the AABB offset before decomposing back
			glm::vec3 new_world_offset = glm::vec3(model * glm::vec4(aabb_offset, 1.0f)) - glm::vec3(model[3]);
			model[3] -= glm::vec4(new_world_offset, 0.0f);

			// Convert back to local space if entity has a parent
			Entity parent = registry->getParent(state.selectedEntity());
			glm::mat4 local_model = model;
			if (!parent.isNull()) {
				const glm::mat4& parent_world = registry->getWorldTransform(parent);
				local_model = glm::inverse(parent_world) * model;
			}

			// Decompose into T/R/S
			glm::vec3 translation, scale, skew;
			glm::vec4 perspective;
			glm::quat rotation;
			glm::decompose(local_model, scale, rotation, translation, skew, perspective);

			if (op == ImGuizmo::TRANSLATE)
				transform->setTranslation(translation);
			else if (op == ImGuizmo::ROTATE)
				transform->setRotation(rotation);
			else if (op == ImGuizmo::SCALE)
				transform->setScale(scale);
		} else {
			// Multi selection
			glm::mat4 delta = model * glm::inverse(model_before);
			for (Entity r : roots) {
				glm::mat4 world_new = delta * registry->getWorldTransform(r);
				Entity parent = registry->getParent(r);
				glm::mat4 local_model = parent.isNull() ? world_new
					: glm::inverse(registry->getWorldTransform(parent)) * world_new;
				glm::vec3 translation, scale, skew;
				glm::vec4 perspective;
				glm::quat rotation;
				glm::decompose(local_model, scale, rotation, translation, skew, perspective);
				auto* tc = registry->getComponent<TransformComponent>(r);
				// Every op moves a satellite about the shared pivot, so translation
				// always updates; only touch the rotation/scale the op actually changes.
				tc->setTranslation(translation);
				if (op == ImGuizmo::ROTATE)
					tc->setRotation(rotation);
				else if (op == ImGuizmo::SCALE)
					tc->setScale(scale);
			}
		}
	}

	state.gizmo_active = ImGuizmo::IsUsing();

	// Freeze physics bodies when gizmo drag starts, unfreeze when it ends
	if (state.gizmo_active && !m_was_gizmo_active && m_physics_system) {
		for (Entity r : roots) {
			m_physics_system->freezeBody(r);
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