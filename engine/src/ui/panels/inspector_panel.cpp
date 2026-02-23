#include "pch.hpp"
#include "ui/panels/inspector_panel.hpp"
#include "ui/imgui_layer.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_material_properties.hpp"
#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>

namespace ve {

// Draws a vec3 editor with colored X/Y/Z labels
// TODO: make fit better
static bool drawVec3Control(const char* label, glm::vec3& values, float speed = 0.1f, float reset_value = 0.0f, int decimals = 2) {
	bool changed = false;
	ImGui::PushID(label);

	char fmt[8];
	snprintf(fmt, sizeof(fmt), "%%.%df", decimals);

	float line_height = ImGui::GetFrameHeight();
	ImVec2 btn_size = {line_height, line_height};

	ImGui::Columns(2, nullptr, false);
	ImGui::SetColumnWidth(0, 85.0f);
	ImGui::Text("%s", label);
	ImGui::NextColumn();

	float total_width = ImGui::GetContentRegionAvail().x;
	float spacing = 1.0f;
	float drag_width = (total_width - btn_size.x * 3 - spacing * 5) / 3.0f;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));

	// X
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
	if (ImGui::Button("X", btn_size))
		{ values.x = reset_value; changed = true; }
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(drag_width);
	if (ImGui::DragFloat("##X", &values.x, speed, 0.0f, 0.0f, fmt))
		changed = true;
	ImGui::SameLine();

	// Y
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.6f, 0.15f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.75f, 0.2f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.6f, 0.15f, 1.0f));
	if (ImGui::Button("Y", btn_size))
		{ values.y = reset_value; changed = true; }
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(drag_width);
	if (ImGui::DragFloat("##Y", &values.y, speed, 0.0f, 0.0f, fmt))
		changed = true;
	ImGui::SameLine();

	// Z
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.7f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.85f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.7f, 1.0f));
	if (ImGui::Button("Z", btn_size))
		{ values.z = reset_value; changed = true; }
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(drag_width);
	if (ImGui::DragFloat("##Z", &values.z, speed, 0.0f, 0.0f, fmt))
		changed = true;

	ImGui::PopStyleVar();
	ImGui::Columns(1);
	ImGui::PopID();
	return changed;
}

void InspectorPanel::render(Registry* registry, EditorState& state, UIContext& /*context*/) {
	if (!ImGui::Begin("Inspector", &state.show_inspector)) {
		ImGui::End();
		return;
	}

	if (state.selected_entity.isNull() || !registry || !registry->isAlive(state.selected_entity)) {
		ImGui::TextDisabled("No entity selected");
		ImGui::End();
		return;
	}

	Entity entity = state.selected_entity;

	renderEntityHeader(*registry, entity);
	ImGui::Separator();

	// Transform
	auto* transform = registry->getComponent<TransformComponent>(entity);
	if (transform) {
		if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
			renderTransform(*transform);
	}

	// Mesh
	auto* mesh = registry->getComponent<MeshComponent>(entity);
	if (mesh) {
		if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
			renderMesh(*mesh);
	}

	// Point Light
	auto* pl = registry->getComponent<PointLightComponent>(entity);
	if (pl) {
		if (ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen))
			renderPointLight(*pl);
	}

	// Directional Light
	auto* dl = registry->getComponent<DirectionalLightComponent>(entity);
	if (dl) {
		if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen))
			renderDirectionalLight(*dl);
	}

	ImGui::End();
}

void InspectorPanel::renderEntityHeader(Registry& registry, Entity entity) {
	// Name
	const std::string& name = registry.getName(entity);
	char buf[256];
	strncpy(buf, name.c_str(), sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	if (ImGui::InputText("Name", buf, sizeof(buf)))
		registry.setName(entity, buf);

	// Active
	bool active = registry.isActive(entity);
	if (ImGui::Checkbox("Active", &active))
		registry.setActive(entity, active);

	// Entity ID
	ImGui::SameLine();
	ImGui::TextDisabled("(ID: %u)", entity.id());
}

void InspectorPanel::renderTransform(TransformComponent& transform) {
	glm::vec3 pos = transform.getTranslation();
	if (drawVec3Control("Translation", pos, 0.1f))
		transform.setTranslation(pos);

	glm::vec3 euler = glm::degrees(glm::eulerAngles(transform.getRotation()));
	if (drawVec3Control("Rotation", euler, 0.5f))
		transform.setRotationEuler(glm::radians(euler));

	glm::vec3 scl = transform.getScale();
	if (drawVec3Control("Scale", scl, 0.01f, 1.0f))
		transform.setScale(scl);
}

void InspectorPanel::renderMesh(MeshComponent& mesh) {
	// Mesh info (read-only)
	VeMesh* m = mesh.getMesh();
	if (m) {
		ImGui::Text("Vertices: %u  Indices: %u  LODs: %u",
			m->getVertexCount(), m->getIndexCount(), m->getLodCount());
	} else {
		ImGui::TextDisabled("No mesh data");
	}

	// Shadow toggle
	ImGui::Checkbox("Cast Shadow", &mesh.has_shadow);

	// Material editing
	VeMaterial* mat = mesh.getMaterial();
	if (!mat) {
		ImGui::TextDisabled("No material");
		return;
	}

	if (!ImGui::TreeNode("Material"))
		return;

	const float label_w = 85.0f;
	auto labeledWidget = [&](const char* text, auto widgetFn) {
		ImGui::Columns(2, nullptr, false);
		ImGui::SetColumnWidth(0, label_w);
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%s", text);
		ImGui::NextColumn();
		ImGui::SetNextItemWidth(-FLT_MIN);
		widgetFn();
		ImGui::Columns(1);
	};

	auto factors = mat->getMaterialFactors();
	bool changed = false;

	labeledWidget("Base Color", [&]() {
		if (ImGui::ColorEdit4("##BaseColor", glm::value_ptr(factors.base_color_factor)))
			changed = true;
	});

	labeledWidget("Metallic", [&]() {
		if (ImGui::SliderFloat("##Metallic", &factors.metallic_factor, 0.0f, 1.0f))
			changed = true;
	});

	labeledWidget("Roughness", [&]() {
		if (ImGui::SliderFloat("##Roughness", &factors.roughness_factor, 0.0f, 1.0f))
			changed = true;
	});

	labeledWidget("Emissive", [&]() {
		if (ImGui::ColorEdit3("##Emissive", glm::value_ptr(factors.emissive_factor)))
			changed = true;
	});

	labeledWidget("Emissive Str", [&]() {
		if (ImGui::DragFloat("##EmissiveStr", &factors.emissive_strength, 0.001f, 0.0f, 100.0f))
			changed = true;
	});

	if (changed)
		mat->setMaterialFactors(factors);

	// Alpha properties
	auto alpha = mat->getAlphaProps();
	bool alpha_changed = false;

	const char* alpha_modes[] = { "Opaque", "Mask", "Blend" };
	int alpha_idx = static_cast<int>(alpha.alpha_mode);
	labeledWidget("Alpha Mode", [&]() {
		if (ImGui::Combo("##AlphaMode", &alpha_idx, alpha_modes, 3)) {
			alpha.alpha_mode = static_cast<AlphaMode>(alpha_idx);
			alpha_changed = true;
		}
	});

	if (alpha.alpha_mode == AlphaMode::MASK) {
		labeledWidget("Alpha Cutoff", [&]() {
			if (ImGui::SliderFloat("##AlphaCutoff", &alpha.alpha_cutoff, 0.0f, 1.0f))
				alpha_changed = true;
		});
	}

	if (ImGui::Checkbox("Double Sided", &alpha.double_sided))
		alpha_changed = true;

	if (alpha_changed)
		mat->setAlphaProps(alpha);

	ImGui::TreePop();
}

void InspectorPanel::renderPointLight(PointLightComponent& light) {
	glm::vec3 color = light.getColor();
	if (ImGui::ColorEdit3("Color", glm::value_ptr(color)))
		light.setColor(color);

	float intensity = light.getIntensity();
	if (ImGui::DragFloat("Intensity", &intensity, 0.1f, 0.0f, 10000.0f))
		light.setIntensity(intensity);

	float range = light.getRange();
	if (ImGui::DragFloat("Range", &range, 0.1f, 0.0f, 1000.0f, "%.1f"))
		light.setRange(range);

	ImGui::Text("Effective Range: %.1f", light.getEffectiveRange());

	bool casts_shadow = light.getCastsShadow();
	if (ImGui::Checkbox("Casts Shadow", &casts_shadow))
		light.setCastsShadow(casts_shadow);

	bool rotates = light.getRotates();
	if (ImGui::Checkbox("Rotates", &rotates))
		light.setRotates(rotates);
}

void InspectorPanel::renderDirectionalLight(DirectionalLightComponent& light) {
	if (drawVec3Control("Direction", light.direction, 0.01f)) {
		float len = glm::length(light.direction);
		if (len > 0.001f)
			light.direction = light.direction / len;
	}

	ImGui::ColorEdit3("Color", glm::value_ptr(light.color));
	ImGui::DragFloat("Intensity", &light.intensity, 0.1f, 0.0f, 100.0f);
	ImGui::Checkbox("Casts Shadow", &light.casts_shadow);

	const char* celestial_types[] = { "Moon", "Sun" };
	int ct = static_cast<int>(light.celestial_type);
	if (ImGui::Combo("Celestial Type", &ct, celestial_types, 2))
		light.celestial_type = static_cast<CelestialType>(ct);
}

} // namespace ve
