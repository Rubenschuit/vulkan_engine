#include "pch.hpp"
#include "ui/panels/viewport_panel.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_camera.hpp"
#include "utils/ve_ray.hpp"
#include "physics/physics_system.hpp"
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace ve {

void ViewportPanel::render(Registry* registry, EditorState& state, UIContext& /*context*/) {
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	bool open = ImGui::Begin("Viewport", &state.show_viewport, ImGuiWindowFlags_NoFocusOnAppearing);
	ImGui::PopStyleVar();

	if (open) {
		state.viewport_hovered = ImGui::IsWindowHovered();
		state.viewport_focused = ImGui::IsWindowFocused();

		renderGizmoToolbar(state);

		ImVec2 size = ImGui::GetContentRegionAvail();
		float dpi_scale = ImGui::GetIO().DisplayFramebufferScale.x;
		state.viewport_width = std::max(0.0f, size.x * dpi_scale);
		state.viewport_height = std::max(0.0f, size.y * dpi_scale);

		// Capture image top-left for gizmo rect (after toolbar, before Image call)
		ImVec2 image_pos = ImGui::GetCursorScreenPos();

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
		    && !state.gizmo_active && !ImGuizmo::IsOver() && m_camera && registry) {
			ImVec2 mouse = ImGui::GetMousePos();
			float uv_x = (mouse.x - image_pos.x) / size.x;
			float uv_y = (mouse.y - image_pos.y) / size.y;

			if (uv_x >= 0.0f && uv_x <= 1.0f && uv_y >= 0.0f && uv_y <= 1.0f) {
				glm::mat4 inv_vp = glm::inverse(m_camera->getProj() * m_camera->getView());
				Ray ray = screenToWorldRay(uv_x, uv_y, inv_vp);
				RayHit hit;
				if (raycastScene(ray, *registry, hit)) {
					state.selected_entity = hit.entity;
					state.selection_changed = true;
				} else if (!state.selected_entity.isNull()) {
					state.selected_entity = Entity::null();
					state.selection_changed = true;
				}
			}
		}

		// Keyboard shortcuts for gizmo mode (when viewport is hovered)
		if (state.viewport_hovered || state.viewport_focused) {
			if (ImGui::IsKeyPressed(ImGuiKey_W) && !ImGui::GetIO().WantTextInput)
				state.gizmo_operation = GizmoOperation::Translate;
			if (ImGui::IsKeyPressed(ImGuiKey_E) && !ImGui::GetIO().WantTextInput)
				state.gizmo_operation = GizmoOperation::Rotate;
			if (ImGui::IsKeyPressed(ImGuiKey_R) && !ImGui::GetIO().WantTextInput)
				state.gizmo_operation = GizmoOperation::Scale;
		}
	}
	ImGui::End();
}

void ViewportPanel::renderGizmoToolbar(EditorState& state) {
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));

	const ImVec4 active_color(0.88f, 0.40f, 0.10f, 1.0f);
	bool is_translate = state.gizmo_operation == GizmoOperation::Translate;
	bool is_rotate = state.gizmo_operation == GizmoOperation::Rotate;
	bool is_scale = state.gizmo_operation == GizmoOperation::Scale;

	if (is_translate) ImGui::PushStyleColor(ImGuiCol_Button, active_color);
	if (ImGui::SmallButton("W Translate"))
		state.gizmo_operation = GizmoOperation::Translate;
	if (is_translate) ImGui::PopStyleColor();

	ImGui::SameLine();
	if (is_rotate) ImGui::PushStyleColor(ImGuiCol_Button, active_color);
	if (ImGui::SmallButton("E Rotate"))
		state.gizmo_operation = GizmoOperation::Rotate;
	if (is_rotate) ImGui::PopStyleColor();

	ImGui::SameLine();
	if (is_scale) ImGui::PushStyleColor(ImGuiCol_Button, active_color);
	if (ImGui::SmallButton("R Scale"))
		state.gizmo_operation = GizmoOperation::Scale;
	if (is_scale) ImGui::PopStyleColor();

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	bool is_world = state.gizmo_space == GizmoSpace::World;
	if (ImGui::SmallButton(is_world ? "World" : "Local"))
		state.gizmo_space = is_world ? GizmoSpace::Local : GizmoSpace::World;

	ImGui::SameLine();
	if (ImGui::SmallButton(state.gizmo_snap_enabled ? "Snap: ON" : "Snap: OFF"))
		state.gizmo_snap_enabled = !state.gizmo_snap_enabled;

	ImGui::PopStyleVar(2);
}

void ViewportPanel::renderGizmo(Registry* registry, EditorState& state, float img_x, float img_y, float img_w, float img_h) {
	state.gizmo_active = false;

	// Unfreeze if gizmo was active last frame but won't be this frame (early return paths)
	auto unfreezeIfNeeded = [&]() {
		if (m_was_gizmo_active && !state.gizmo_active && !m_frozen_entity.isNull() && m_physics_system) {
			m_physics_system->unfreezeBody(m_frozen_entity);
			m_frozen_entity = Entity::null();
		}
		m_was_gizmo_active = state.gizmo_active;
	};

	if (!m_camera || !registry || state.selected_entity.isNull()) {
		unfreezeIfNeeded();
		return;
	}
	if (!registry->isAlive(state.selected_entity)) {
		unfreezeIfNeeded();
		return;
	}

	auto* transform = registry->getComponent<TransformComponent>(state.selected_entity);
	if (!transform) {
		unfreezeIfNeeded();
		return;
	}

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
	const glm::mat4& world = registry->getWorldTransform(state.selected_entity);
	glm::mat4 model = world;
	const glm::mat4& view = m_camera->getView();

	glm::vec3 aabb_offset = state.cached_aabb_offset;

	// Apply offset in local space
	glm::vec3 world_offset = glm::vec3(model * glm::vec4(aabb_offset, 1.0f)) - glm::vec3(model[3]);
	model[3] += glm::vec4(world_offset, 0.0f);

	// ImGuizmo expects a standard forward-Z projection
	// Build one from camera parameters since our actual proj is infinite reverse-Z.
	glm::mat4 gizmo_proj = glm::perspective(m_camera->getFovY(), m_camera->getAspect(),
	                                         m_camera->getNear(), m_camera->getFar());

	if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(gizmo_proj),
	                         op, mode, glm::value_ptr(model), nullptr, snap_ptr)) {
		// Remove the AABB offset before decomposing back
		glm::vec3 new_world_offset = glm::vec3(model * glm::vec4(aabb_offset, 1.0f)) - glm::vec3(model[3]);
		model[3] -= glm::vec4(new_world_offset, 0.0f);

		// Convert back to local space if entity has a parent
		Entity parent = registry->getParent(state.selected_entity);
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
	}

	state.gizmo_active = ImGuizmo::IsUsing();

	// Freeze physics body when gizmo drag starts, unfreeze when it ends
	if (state.gizmo_active && !m_was_gizmo_active && m_physics_system) {
		m_physics_system->freezeBody(state.selected_entity);
		m_frozen_entity = state.selected_entity;
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
	if (!m_physics_system || !m_camera || !registry || state.selected_entity.isNull())
		return;

	auto shape = m_physics_system->getDebugShape(state.selected_entity, *registry);
	if (!shape)
		return;

	glm::mat4 vp = m_camera->getProj() * m_camera->getView();
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