#include "pch.hpp"
#include "ui/panels/hierarchy_panel.hpp"
#include "ui/imgui_layer.hpp"
#include "scene/ve_registry.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <map>

namespace ve {

bool HierarchyPanel::isLightOnly(Registry& registry, Entity entity) {
	bool is_light = registry.hasComponent<PointLightComponent>(entity)
	             || registry.hasComponent<DirectionalLightComponent>(entity);
	return is_light && !registry.hasComponent<MeshComponent>(entity);
}

void HierarchyPanel::render(Registry* registry, EditorState& state, UIContext& /*context*/) {
	if (!ImGui::Begin("Scene Hierarchy", &state.show_hierarchy)) {
		ImGui::End();
		return;
	}

	// App-injected header (scene selection)
	if (m_header_callback) {
		m_header_callback();
		ImGui::Separator();
		ImGui::Spacing();
	}

	if (!registry) {
		ImGui::Text("No scene loaded");
		ImGui::End();
		m_last_registry = nullptr;
		return;
	}

	if (registry != m_last_registry) {
		m_group_states.clear();
		m_last_registry = registry;
	}

	// Lights section
	renderLightsSection(*registry, state);

	// Entity tree (skip light-only entities)
	if (ImGui::CollapsingHeader("Entities", ImGuiTreeNodeFlags_DefaultOpen)) {
		uint32_t max_idx = registry->maxEntityIndex();
		for (uint32_t i = 0; i < max_idx; ++i) {
			if (!registry->isAliveAtIndex(i))
				continue;
			Entity e = registry->entityFromIndex(i);
			if (registry->hasParent(e))
				continue;
			if (isLightOnly(*registry, e))
				continue;
			renderEntityNode(*registry, e, state);
		}
	}

	// Click empty space to deselect
	if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
		state.selected_entity = Entity::null();
		state.selection_changed = true;
	}

	// Delete key on selected entity
	if (!state.selected_entity.isNull() && ImGui::IsWindowFocused()
	    && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
		m_pending_delete = state.selected_entity;

	// Schedule deferred deletion via event system (actual destroy happens at safe frame boundary)
	if (!m_pending_delete.isNull() && registry) {
		if (state.selected_entity == m_pending_delete) {
			state.selected_entity = Entity::null();
			state.selection_changed = true;
		}
		registry->events().emit(DeleteEntityRequest{m_pending_delete, /*recursive=*/true});
		m_pending_delete = Entity::null();
	}

	ImGui::End();
}

void HierarchyPanel::renderEntityNode(Registry& registry, Entity entity, EditorState& state) {
	uint32_t idx = entity.index();

	// Build display name
	const std::string& name = registry.getName(entity);
	char label[256];
	if (name.empty())
		snprintf(label, sizeof(label), "Entity %u", idx);
	else
		snprintf(label, sizeof(label), "%s", name.c_str());

	// Component badges
	bool has_mesh = registry.hasComponent<MeshComponent>(entity);
	bool has_pl = registry.hasComponent<PointLightComponent>(entity);
	bool has_dl = registry.hasComponent<DirectionalLightComponent>(entity);

	// Check if has visible (non-light-only) children
	bool has_visible_children = false;
	Entity child = registry.firstChild(entity);
	while (!child.isNull()) {
		if (!isLightOnly(registry, child)) {
			has_visible_children = true;
			break;
		}
		child = registry.nextSibling(child);
	}

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (!has_visible_children)
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (state.selected_entity == entity)
		flags |= ImGuiTreeNodeFlags_Selected;

	// Dim inactive entities
	bool active = registry.isActive(entity);
	if (!active)
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

	bool node_open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entity.id())), flags, "%s", label);

	// Capture TreeNode click state and context menu before badge widgets shift the "last item"
	bool tree_clicked = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
	if (ImGui::BeginPopupContextItem()) {
		if (ImGui::MenuItem("Delete"))
			m_pending_delete = entity;
		ImGui::EndPopup();
	}

	if (!active)
		ImGui::PopStyleColor();

	// Component badges on the same line
	if (has_mesh || has_pl || has_dl) {
		ImGui::SameLine();
		if (has_mesh) {
			ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "[M]");
			if (has_pl || has_dl) ImGui::SameLine();
		}
		if (has_pl) {
			ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "[PL]");
			if (has_dl) ImGui::SameLine();
		}
		if (has_dl)
			ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "[DL]");
	}

	// Handle selection
	if (tree_clicked) {
		state.selected_entity = entity;
		state.selection_changed = true;
	}

	// Render children recursively (skip light-only)
	if (has_visible_children && node_open) {
		child = registry.firstChild(entity);
		while (!child.isNull()) {
			if (!isLightOnly(registry, child))
				renderEntityNode(registry, child, state);
			child = registry.nextSibling(child);
		}
		ImGui::TreePop();
	}
}

void HierarchyPanel::renderSelectableLight(Registry& registry, Entity entity, EditorState& state) {
	const std::string& name = registry.getName(entity);
	char label[256];
	if (name.empty())
		snprintf(label, sizeof(label), "Light %u", entity.index());
	else
		snprintf(label, sizeof(label), "%s", name.c_str());

	bool active = registry.isActive(entity);
	if (!active)
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
	                         | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (state.selected_entity == entity)
		flags |= ImGuiTreeNodeFlags_Selected;

	ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entity.id())), flags, "%s", label);

	if (ImGui::BeginPopupContextItem()) {
		if (ImGui::MenuItem("Delete"))
			m_pending_delete = entity;
		ImGui::EndPopup();
	}

	if (ImGui::IsItemClicked()) {
		state.selected_entity = entity;
		state.selection_changed = true;
	}

	if (!active)
		ImGui::PopStyleColor();
}

void HierarchyPanel::renderEnableCheckbox(const char* label, const std::vector<Entity>& lights, Registry& registry) {
	int active_count = 0;
	for (auto e : lights)
		if (registry.isActive(e))
			active_count++;

	bool all_active = (active_count == static_cast<int>(lights.size()));
	bool mixed = active_count > 0 && !all_active;
	if (mixed)
		ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
	if (ImGui::Checkbox(label, &all_active))
		for (auto e : lights)
			registry.setActive(e, all_active);
	if (mixed)
		ImGui::PopItemFlag();
}

void HierarchyPanel::renderGroupControls(const std::string& key, const std::vector<Entity>& lights, Registry& registry) {
	if (lights.empty()) return;

	auto& state = m_group_states[key];

	// Lazy-initialize from first light
	if (!state.initialized) {
		auto* pl = registry.getComponent<PointLightComponent>(lights[0]);
		if (pl) {
			state.color = pl->getColor();
			state.range = pl->getRange();
		}
		state.initialized = true;
	}

	// Intensity multiplier
	ImGui::Text("Intensity");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(70.0f);
	float old_mult = state.intensity_multiplier;
	if (ImGui::DragFloat("##intensity", &state.intensity_multiplier, 0.01f, 0.01f, 10.0f, "%.2fx")) {
		if (old_mult > 0.001f) {
			float ratio = state.intensity_multiplier / old_mult;
			for (auto e : lights) {
				auto* pl = registry.getComponent<PointLightComponent>(e);
				if (pl)
					pl->setIntensity(pl->getIntensity() * ratio);
			}
		}
	}

	// Color override
	ImGui::SameLine();
	ImGui::Text("Color");
	ImGui::SameLine();
	if (ImGui::ColorEdit3("##color", glm::value_ptr(state.color),
			ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
		for (auto e : lights) {
			auto* pl = registry.getComponent<PointLightComponent>(e);
			if (pl)
				pl->setColor(state.color);
		}
	}

	// Range override
	ImGui::SameLine();
	ImGui::Text("Range");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(70.0f);
	if (ImGui::DragFloat("##range", &state.range, 0.1f, 0.0f, 1000.0f, "%.1f")) {
		for (auto e : lights) {
			auto* pl = registry.getComponent<PointLightComponent>(e);
			if (pl)
				pl->setRange(state.range);
		}
	}
}

void HierarchyPanel::renderLightsSection(Registry& registry, EditorState& state) {
	uint32_t dl_count = registry.directionalLights().size();
	uint32_t pl_count = registry.pointLights().size();
	if (dl_count == 0 && pl_count == 0)
		return;

	char header[64];
	snprintf(header, sizeof(header), "Lights (%u)", dl_count + pl_count);
	if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
		return;

	ImGui::PushID("lights_section");

	// --- Directional Lights ---
	if (dl_count > 0 && ImGui::TreeNodeEx("Directional", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (auto [e, dl] : registry.view<DirectionalLightComponent>().includeInactive())
			renderSelectableLight(registry, e, state);
		ImGui::TreePop();
	}

	// --- Point Lights ---
	if (pl_count > 0 && ImGui::TreeNodeEx("Point Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		// Collect and sort for stable UI order
		auto pl_view = registry.view<PointLightComponent, TransformComponent>().includeInactive();
		std::vector<Entity> lights;
		lights.reserve(pl_view.sizeHint());
		for (auto [e, pl, tc] : pl_view)
			lights.push_back(e);
		std::sort(lights.begin(), lights.end());

		// Top-level enable checkbox + group controls
		if (!lights.empty()) {
			renderEnableCheckbox("Enable all", lights, registry);
			renderGroupControls("all_pl", lights, registry);
		}

		// Partition by LightSource
		std::vector<Entity> scene_lights, punctual_lights, emissive_lights;
		for (auto e : lights) {
			switch (registry.getLightSource(e)) {
				case LightSource::Punctual: punctual_lights.push_back(e); break;
				case LightSource::Emissive: emissive_lights.push_back(e); break;
				default:                    scene_lights.push_back(e); break;
			}
		}

		// Manual/scene lights listed directly
		for (auto e : scene_lights)
			renderSelectableLight(registry, e, state);

		// Source groups (Punctual, Emissive)
		struct SourceGroup { const char* label; std::vector<Entity>& entities; };
		SourceGroup source_groups[] = {
			{"KHR Punctual", punctual_lights},
			{"Emissive",     emissive_lights},
		};
		for (auto& [group_label, entities] : source_groups) {
			if (entities.empty()) continue;
			ImGui::PushID(group_label);

			char group_header[64];
			snprintf(group_header, sizeof(group_header), "%s (%zu)", group_label, entities.size());
			if (ImGui::TreeNode(group_header)) {
				renderEnableCheckbox("Enable all", entities, registry);
				renderGroupControls(group_label, entities, registry);
				renderLightGroup(registry, group_label, entities, state);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		ImGui::TreePop();
	}

	ImGui::PopID();
}

void HierarchyPanel::renderLightGroup(Registry& registry, const std::string& source_key, const std::vector<Entity>& group_lights, EditorState& state) {
	// Sub-partition by name prefix (before ": ")
	std::map<std::string, std::vector<Entity>> sub_groups;
	for (auto e : group_lights) {
		const auto& name = registry.getName(e);
		auto sep = name.find(": ");
		if (sep != std::string::npos)
			sub_groups[name.substr(0, sep)].push_back(e);
		else
			sub_groups["Ungrouped"].push_back(e);
	}

	// Ungrouped lights listed directly
	auto ungrouped_it = sub_groups.find("Ungrouped");
	if (ungrouped_it != sub_groups.end())
		for (auto e : ungrouped_it->second)
			renderSelectableLight(registry, e, state);

	// Named sub-groups with enable toggle + group controls
	for (auto& [sub_name, sub_lights] : sub_groups) {
		if (sub_name == "Ungrouped") continue;
		ImGui::PushID(sub_name.c_str());

		char sub_header[128];
		snprintf(sub_header, sizeof(sub_header), "%s (%zu)", sub_name.c_str(), sub_lights.size());
		if (ImGui::TreeNode(sub_header)) {
			renderEnableCheckbox("Enable group", sub_lights, registry);
			renderGroupControls(source_key + "/" + sub_name, sub_lights, registry);

			for (auto e : sub_lights)
				renderSelectableLight(registry, e, state);

			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}

} // namespace ve