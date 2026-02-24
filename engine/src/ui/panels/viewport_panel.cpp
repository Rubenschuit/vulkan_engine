#include "pch.hpp"
#include "ui/panels/viewport_panel.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_camera.hpp"
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace ve {

void ViewportPanel::render(Registry* registry, EditorState& state, UIContext& /*context*/) {
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	bool open = ImGui::Begin("Viewport", &state.show_viewport);
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

	if (!m_camera || !registry || state.selected_entity.isNull())
		return;
	if (!registry->isAlive(state.selected_entity))
		return;

	auto* transform = registry->getComponent<TransformComponent>(state.selected_entity);
	if (!transform)
		return;

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

	// Undo Vulkan Y-flip, ImGuizmo expects OpenGL clip-space convention
	glm::mat4 gizmo_proj = m_camera->getProj();
	gizmo_proj[1][1] *= -1.0f;

	if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(gizmo_proj),
	                         op, mode, glm::value_ptr(model), nullptr, snap_ptr)) {
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

		transform->setTranslation(translation);
		transform->setRotation(rotation);
		transform->setScale(scale);
	}

	state.gizmo_active = ImGuizmo::IsUsing();
}

} // namespace ve