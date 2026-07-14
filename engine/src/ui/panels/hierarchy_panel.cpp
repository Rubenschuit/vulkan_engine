#include "pch.hpp"
#include "ui/panels/hierarchy_panel.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/editor_icons.hpp"
#include "application/ve_application.hpp"
#include "events/engine_events.hpp"
#include "resources/asset_loading_system.hpp"
#include "scene/scene_manager.hpp"
#include "scene/scene_overlay.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_scene.hpp"
#include "utils/ve_path.hpp"
#include "utils/ve_string.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cstdint>
#include <map>

namespace ve {

bool HierarchyPanel::matchesNameSearch(Registry& registry, Entity entity) {
	if (!m_search_active)
		return true;
	const std::string& name = registry.getName(entity);
	return std::search(name.begin(), name.end(), m_search_buf, m_search_buf + std::strlen(m_search_buf),
		ve::iequalsChar) != name.end();
}

bool HierarchyPanel::matchesTypeFilter(Registry& registry, Entity entity) {
	switch (m_filter) {
		case TreeFilter::Meshes:  return registry.hasComponent<MeshComponent>(entity);
		case TreeFilter::Lights:  return registry.hasComponent<PointLightComponent>(entity)
		                              || registry.hasComponent<SpotLightComponent>(entity)
		                              || registry.hasComponent<DirectionalLightComponent>(entity)
		                              || registry.hasComponent<AreaLightComponent>(entity);
		case TreeFilter::Cameras: return registry.hasComponent<CameraComponent>(entity);
		default:                  return true;
	}
}

// A node is shown if it matches both filters, or has any descendant that does
bool HierarchyPanel::subtreeVisible(Registry& registry, Entity entity) {
	if (matchesTypeFilter(registry, entity) && matchesNameSearch(registry, entity))
		return true;
	Entity child = registry.firstChild(entity);
	while (!child.isNull()) {
		if (subtreeVisible(registry, child))
			return true;
		child = registry.nextSibling(child);
	}
	return false;
}

void HierarchyPanel::render(Registry* registry, EditorState& state, UIContext& /*context*/) {
	if (!ImGui::Begin("Scene Hierarchy", &state.show_hierarchy, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	// Scene management (scene selector, GLTF loading)
	renderSceneSelector();

	if (!registry) {
		ImGui::Text("No scene loaded");
		ImGui::End();
		m_last_registry = nullptr;
		return;
	}

	if (registry != m_last_registry) {
		m_group_states.clear();
		m_last_registry = registry;
		state.clearSelection();
		m_renaming_entity = Entity::null();
	}

	// Deferred deletion can kill entities between frames
	if (!state.selected_entities.empty()) {
		auto& sel = state.selected_entities;
		sel.erase(std::remove_if(sel.begin(), sel.end(),
			[&](Entity e) { return !registry->isAlive(e); }), sel.end());
	}
	if (!m_renaming_entity.isNull() && !registry->isAlive(m_renaming_entity))
		m_renaming_entity = Entity::null();

	m_joint_entity_ids.clear();
	const auto& skin_pool = registry->skins();
	for (uint32_t i = 0; i < skin_pool.size(); i++) {
		const SkinComponent& sc = skin_pool.data()[i];
		for (Entity je : sc.getJointEntities())
			if (!je.isNull())
				m_joint_entity_ids.insert(je.id());
	}

	// Auto-expand ancestors when selection changes
	if (state.selection_changed && !state.selectedEntity().isNull()) {
		m_force_open_entities.clear();
		Entity ancestor = registry->getParent(state.selectedEntity());
		while (!ancestor.isNull()) {
			m_force_open_entities.insert(ancestor.id());
			ancestor = registry->getParent(ancestor);
		}
		m_scroll_to_selected = true;
	}

	// Search field with clear button
	float clear_w = m_search_active ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
	ImGui::SetNextItemWidth(-FLT_MIN - clear_w);
	ImGui::InputTextWithHint("##search", "Search...", m_search_buf, sizeof(m_search_buf));
	m_search_active = m_search_buf[0] != '\0';
	if (m_search_active) {
		ImGui::SameLine();
		if (ImGui::Button("X##clear_search", ImVec2(ImGui::GetFrameHeight(), 0))) {
			m_search_buf[0] = '\0';
			m_search_active = false;
		}
	}

	// Type filters
	renderFilters(*registry);

	// F2 renames the primary selection
	if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput
	    && !state.selectedEntity().isNull() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
		m_renaming_entity = state.selectedEntity();
		snprintf(m_rename_buf, sizeof(m_rename_buf), "%s", registry->getName(state.selectedEntity()).c_str());
		m_rename_focus = true;
	}

	// Ctrl+G groups the selection
	if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && !state.selected_entities.empty()
	    && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G))
		m_pending_group = state.selected_entities;

	if (ImGui::Button("New Entity"))
		m_pending_create_empty = true;

	ImGui::Separator();

	ImGuiMultiSelectFlags ms_flags = ImGuiMultiSelectFlags_ClearOnEscape
		| ImGuiMultiSelectFlags_ClearOnClickVoid | ImGuiMultiSelectFlags_BoxSelect2d;
	ImGuiMultiSelectIO* ms = ImGui::BeginMultiSelect(ms_flags, static_cast<int>(state.selected_entities.size()), -1);
	applySelectionRequests(ms, *registry, state);

	m_visible_order.clear();
	m_visible_row_index = 0;

	if (m_filter == TreeFilter::Lights) {
		// Grouped light management view
		renderSaveLightsButton(*registry);
		renderLightGroups(*registry, state);
	} else if (m_filter != TreeFilter::All || m_search_active) {
		// Flat list of matches (Meshes/Cameras, or a search within All)
		renderFlatList(*registry, state);
	} else {
		// Scene hierarchy (collapse state preserved; never force-opened by filters)
		uint32_t max_idx = registry->maxEntityIndex();
		for (uint32_t i = 0; i < max_idx; ++i) {
			if (!registry->isAliveAtIndex(i))
				continue;
			Entity e = registry->entityFromIndex(i);
			if (registry->hasParent(e))
				continue;
			renderEntityNode(*registry, e, state);
		}
	}

	ms = ImGui::EndMultiSelect();
	applySelectionRequests(ms, *registry, state);

	// Delete key on the whole selection
	if (!state.selected_entities.empty() && ImGui::IsWindowFocused()
	    && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
		m_pending_deletes = state.selected_entities;

	m_force_open_entities.clear();

	// Schedule deferred deletion
	if (!m_pending_deletes.empty() && registry) {
		for (Entity e : m_pending_deletes) {
			state.removeFromSelection(e);
			registry->events().emit(DeleteEntityRequest{e, /*recursive=*/true});
		}
		state.selection_changed = true;
		m_pending_deletes.clear();
	}

	// Duplicate the selection recursively
	if (!m_pending_duplicates.empty() && registry) {
		state.selected_entities.clear();
		for (Entity e : topMostRoots(*registry, m_pending_duplicates))
			state.selected_entities.push_back(registry->cloneEntityRecursive(e));
		state.selection_changed = true;
		m_pending_duplicates.clear();
	}

	// Group / ungroup
	if (!m_pending_group.empty() && registry) {
		groupEntities(*registry, state, m_pending_group);
		m_pending_group.clear();
	}
	if (!m_pending_ungroup.empty() && registry) {
		for (Entity g : m_pending_ungroup)
			ungroupEntity(*registry, state, g);
		m_pending_ungroup.clear();
	}

	// Create a child entity (transform only) under the chosen parent and select it
	if (!m_pending_create_child.isNull() && registry) {
		Entity child = registry->createGameObject("Entity");
		registry->setParent(child, m_pending_create_child);
		state.selected_entities.clear();
		state.selected_entities.push_back(child);
		state.selection_changed = true;
		m_pending_create_child = Entity::null();
	}

	// Create a top-level empty entity (transform only) and select it
	if (m_pending_create_empty && registry) {
		Entity e = registry->createGameObject("Entity");
		state.selected_entities.clear();
		state.selected_entities.push_back(e);
		state.selection_changed = true;
		m_pending_create_empty = false;
	}

	ImGui::End();
}

namespace {

enum class EntityKind { Group, Mesh, PointLight, SpotLight, DirLight, AreaLight, Camera, Particle, Joint, Skin };

EntityKind primaryKind(Registry& r, Entity e, bool is_joint) {
	if (r.hasComponent<CameraComponent>(e)) return EntityKind::Camera;
	if (r.hasComponent<DirectionalLightComponent>(e)) return EntityKind::DirLight;
	if (r.hasComponent<SpotLightComponent>(e)) return EntityKind::SpotLight;
	if (r.hasComponent<PointLightComponent>(e)) return EntityKind::PointLight;
	if (r.hasComponent<AreaLightComponent>(e)) return EntityKind::AreaLight;
	if (r.hasComponent<MeshComponent>(e)) return EntityKind::Mesh;
	if (r.hasComponent<ParticleEmitterComponent>(e)) return EntityKind::Particle;
	if (is_joint) return EntityKind::Joint;
	if (r.hasComponent<SkinComponent>(e)) return EntityKind::Skin;
	return EntityKind::Group;
}

const char* kindIcon(EntityKind k) {
	switch (k) {
		case EntityKind::Camera:     return ICON_CAMERA;
		case EntityKind::DirLight:   return ICON_DIR_LIGHT;
		case EntityKind::SpotLight:  return ICON_SPOT_LIGHT;
		case EntityKind::PointLight: return ICON_POINT_LIGHT;
		case EntityKind::AreaLight:  return ICON_AREA_LIGHT;
		case EntityKind::Mesh:       return ICON_MESH;
		case EntityKind::Particle:   return ICON_PARTICLE;
		case EntityKind::Joint:      return ICON_BONE;
		case EntityKind::Skin:       return ICON_SKIN;
		default:                     return ICON_GROUP;
	}
}

} // namespace

void HierarchyPanel::applySelectionRequests(ImGuiMultiSelectIO* ms, Registry& registry, EditorState& state) {
	if (!ms)
		return;
	for (const ImGuiSelectionRequest& req : ms->Requests) {
		if (req.Type == ImGuiSelectionRequestType_SetAll) {
			state.selected_entities.clear();
			if (req.Selected)
				for (Entity e : m_visible_order)
					if (registry.isAlive(e))
						state.selected_entities.push_back(e);
			state.selection_changed = true;
		} else if (req.Type == ImGuiSelectionRequestType_SetRange) {
			uint32_t first_id = static_cast<uint32_t>(req.RangeFirstItem);
			uint32_t last_id = static_cast<uint32_t>(req.RangeLastItem);
			int i0 = -1, i1 = -1;
			for (int i = 0; i < static_cast<int>(m_visible_order.size()); ++i) {
				if (m_visible_order[static_cast<size_t>(i)].id() == first_id)
					i0 = i;
				if (m_visible_order[static_cast<size_t>(i)].id() == last_id)
					i1 = i;
			}
			if (i0 < 0 || i1 < 0)
				continue;
			if (i0 > i1)
				std::swap(i0, i1);
			for (int i = i0; i <= i1; ++i) {
				Entity e = m_visible_order[static_cast<size_t>(i)];
				if (req.Selected) {
					if (registry.isAlive(e) && !state.isSelected(e))
						state.selected_entities.push_back(e);
				} else {
					state.removeFromSelection(e);
				}
			}
			state.selection_changed = true;
		}
	}
}

// Wrap the selected roots (those with no selected ancestor) in a new empty entity
// placed at their world centroid, preserving each child's world transform.
void HierarchyPanel::groupEntities(Registry& registry, EditorState& state, const std::vector<Entity>& targets) {
	std::vector<Entity> roots = topMostRoots(registry, targets);
	if (roots.empty())
		return;

	// Group inherits a shared parent only if every root has the same one
	Entity common_parent = registry.getParent(roots[0]);
	for (size_t i = 1; i < roots.size(); ++i)
		if (registry.getParent(roots[i]) != common_parent) {
			common_parent = Entity::null();
			break;
		}

	glm::vec3 centroid(0.0f);
	for (Entity e : roots)
		centroid += glm::vec3(registry.getWorldTransform(e)[3]);
	centroid /= static_cast<float>(roots.size());

	Entity group = registry.createGameObject("Group");
	if (auto* tc = registry.getComponent<TransformComponent>(group))
		tc->setTranslation(centroid);

	for (Entity e : roots)
		registry.reparent(e, group);
	if (!common_parent.isNull() && registry.isAlive(common_parent))
		registry.reparent(group, common_parent);

	state.selectSingle(group);
}

// Dissolve a group: reparent its children to the group's parent (world preserved), then delete it.
void HierarchyPanel::ungroupEntity(Registry& registry, EditorState& state, Entity group) {
	if (!registry.isAlive(group))
		return;
	Entity parent = registry.getParent(group);

	std::vector<Entity> children;
	for (Entity c = registry.firstChild(group); !c.isNull(); c = registry.nextSibling(c))
		children.push_back(c);
	for (Entity c : children)
		registry.reparent(c, parent);

	state.removeFromSelection(group);
	registry.events().emit(DeleteEntityRequest{group, /*recursive=*/true});

	state.selected_entities = children;
	state.selection_changed = true;
}

void HierarchyPanel::renderEntityNode(Registry& registry, Entity entity, EditorState& state, bool flat) {
	m_visible_order.push_back(entity);
	uint32_t idx = entity.index();
	const std::string& name = registry.getName(entity);

	bool has_mesh = registry.hasComponent<MeshComponent>(entity);
	bool has_pl = registry.hasComponent<PointLightComponent>(entity);
	bool has_sl = registry.hasComponent<SpotLightComponent>(entity);
	bool has_dl = registry.hasComponent<DirectionalLightComponent>(entity);
	bool has_al = registry.hasComponent<AreaLightComponent>(entity);
	bool has_anim = registry.hasComponent<AnimatorComponent>(entity);
	bool has_skin = registry.hasComponent<SkinComponent>(entity);
	bool has_cam = registry.hasComponent<CameraComponent>(entity);
	bool has_emitter = registry.hasComponent<ParticleEmitterComponent>(entity);
	bool is_joint = m_joint_entity_ids.count(entity.id()) != 0;
	EntityKind pk = primaryKind(registry, entity, is_joint);

	bool has_visible_children = false;
	if (!flat) {
		Entity child = registry.firstChild(entity);
		while (!child.isNull()) {
			if (subtreeVisible(registry, child)) {
				has_visible_children = true;
				break;
			}
			child = registry.nextSibling(child);
		}
	}

	bool renaming = (m_renaming_entity == entity);
	char label[300];
	if (renaming)
		snprintf(label, sizeof(label), "%s", kindIcon(pk));
	else if (name.empty())
		snprintf(label, sizeof(label), "%s  Entity %u", kindIcon(pk), idx);
	else
		snprintf(label, sizeof(label), "%s  %s", kindIcon(pk), name.c_str());

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap;
	if (!has_visible_children)
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (state.isSelected(entity))
		flags |= ImGuiTreeNodeFlags_Selected;

	if (m_force_open_entities.count(entity.id()))
		ImGui::SetNextItemOpen(true);

	// Zebra background behind alternate rows (drawn before the node, so text sits on top)
	ImVec2 row_min = ImGui::GetCursorScreenPos();
	if ((m_visible_row_index++ & 1) != 0) {
		float fl = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
		float fr = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		float h = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(fl, row_min.y), ImVec2(fr, row_min.y + h),
			ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
	}

	bool eff_active = registry.isActiveInHierarchy(entity);
	if (!eff_active)
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

	ImGui::SetNextItemSelectionUserData(static_cast<ImGuiSelectionUserData>(entity.id()));
	bool node_open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entity.id())), flags, "%s", label);
	bool node_hovered = ImGui::IsItemHovered();
	bool row_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

	if (!renaming && node_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
		m_renaming_entity = entity;
		snprintf(m_rename_buf, sizeof(m_rename_buf), "%s", name.c_str());
		m_rename_focus = true;
		renaming = true;
	}

	if (ImGui::BeginPopupContextItem()) {
		bool in_sel = state.isSelected(entity);
		bool many = in_sel && state.selected_entities.size() > 1;
		if (ImGui::MenuItem("Create Child Entity"))
			m_pending_create_child = entity;
		ImGui::Separator();
		if (ImGui::MenuItem(many ? "Group selected" : "Group", "Ctrl+G"))
			m_pending_group = in_sel ? state.selected_entities : std::vector<Entity>{entity};
		if (pk == EntityKind::Group && !registry.firstChild(entity).isNull())
			if (ImGui::MenuItem("Ungroup"))
				m_pending_ungroup = {entity};
		ImGui::Separator();
		if (ImGui::MenuItem(many ? "Duplicate selected" : "Duplicate"))
			m_pending_duplicates = in_sel ? state.selected_entities : std::vector<Entity>{entity};
		if (ImGui::MenuItem(many ? "Delete selected" : "Delete"))
			m_pending_deletes = in_sel ? state.selected_entities : std::vector<Entity>{entity};
		ImGui::Separator();
		if (ImGui::MenuItem("Set Active"))
			for (Entity e : (in_sel ? state.selected_entities : std::vector<Entity>{entity}))
				registry.setActive(e, true);
		if (ImGui::MenuItem("Set Inactive"))
			for (Entity e : (in_sel ? state.selected_entities : std::vector<Entity>{entity}))
				registry.setActive(e, false);
		ImGui::EndPopup();
	}

	if (!eff_active)
		ImGui::PopStyleColor();

	if (renaming) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		if (m_rename_focus) {
			ImGui::SetKeyboardFocusHere();
			m_rename_focus = false;
		}
		if (ImGui::InputText("##rename", m_rename_buf, sizeof(m_rename_buf),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
			registry.setName(entity, m_rename_buf);
			m_renaming_entity = Entity::null();
		}
		if (ImGui::IsItemDeactivated())
			m_renaming_entity = Entity::null();
	} else {
		struct Badge { bool show; const char* icon; const char* name; };
		Badge badges[] = {
			{has_mesh && pk != EntityKind::Mesh,        ICON_MESH,        "Mesh"},
			{has_anim,                                  ICON_ANIMATOR,    "Animator"},
			{has_skin && pk != EntityKind::Skin,        ICON_SKIN,        "Skin"},
			{is_joint && pk != EntityKind::Joint,       ICON_BONE,        "Joint"},
			{has_pl && pk != EntityKind::PointLight,    ICON_POINT_LIGHT, "Point light"},
			{has_sl && pk != EntityKind::SpotLight,     ICON_SPOT_LIGHT,  "Spot light"},
			{has_dl && pk != EntityKind::DirLight,      ICON_DIR_LIGHT,   "Directional light"},
			{has_al && pk != EntityKind::AreaLight,     ICON_AREA_LIGHT,  "Area light"},
			{has_cam && pk != EntityKind::Camera,       ICON_CAMERA,      "Camera"},
			{has_emitter && pk != EntityKind::Particle, ICON_PARTICLE,    "Emitter"},
		};
		if (!eff_active)
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, ImGui::GetStyle().ItemSpacing.y));
		for (auto& b : badges) {
			if (!b.show)
				continue;
			ImGui::SameLine();
			ImGui::TextUnformatted(b.icon);
			ImGui::SetItemTooltip("%s", b.name);
		}
		ImGui::PopStyleVar();
		if (!eff_active)
			ImGui::PopStyleColor();

		renderVisibilityToggle(registry, entity, row_hovered);
	}

	if (state.selectedEntity() == entity && m_scroll_to_selected) {
		ImGui::SetScrollHereY();
		m_scroll_to_selected = false;
	}

	if (has_visible_children && node_open) {
		float guide_x = row_min.x + ImGui::GetTreeNodeToLabelSpacing() * 0.5f;
		float guide_top = ImGui::GetCursorScreenPos().y;
		Entity child = registry.firstChild(entity);
		while (!child.isNull()) {
			if (subtreeVisible(registry, child))
				renderEntityNode(registry, child, state);
			child = registry.nextSibling(child);
		}
		float guide_bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
		ImGui::GetWindowDrawList()->AddLine(ImVec2(guide_x, guide_top), ImVec2(guide_x, guide_bottom),
			IM_COL32(130, 130, 130, 55), 1.0f);
		ImGui::TreePop();
	}
}

void HierarchyPanel::renderVisibilityToggle(Registry& registry, Entity entity, bool row_hovered) {
	bool self_active = registry.isActive(entity);
	bool eff_active = registry.isActiveInHierarchy(entity);
	if (!row_hovered && self_active && eff_active)
		return;

	float sz = ImGui::GetTextLineHeight();
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - sz);
	ImGui::PushID(static_cast<int>(entity.id()));

	bool ancestor_hidden = self_active && !eff_active;
	if (ancestor_hidden)
		ImGui::BeginDisabled();
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.10f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.16f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
	if (ImGui::Button(self_active ? ICON_EYE : ICON_EYE_OFF, ImVec2(sz, sz)))
		registry.setActive(entity, !self_active);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(3);
	if (ancestor_hidden)
		ImGui::EndDisabled();
	if (!ancestor_hidden && ImGui::IsItemHovered())
		ImGui::SetTooltip(self_active ? "Hide" : "Show");

	ImGui::PopID();
}

void HierarchyPanel::renderFilters(Registry& registry) {
	uint32_t n_all = registry.entityCount();
	uint32_t n_mesh = registry.meshes().size();
	uint32_t n_light = registry.pointLights().size() + registry.spotLights().size()
	                 + registry.directionalLights().size();
	uint32_t n_cam = registry.cameras().size();

	struct Filter { TreeFilter mode; const char* name; uint32_t count; };
	Filter filters[] = {
		{TreeFilter::All,     "All",     n_all},
		{TreeFilter::Meshes,  "Meshes",  n_mesh},
		{TreeFilter::Lights,  "Lights",  n_light},
		{TreeFilter::Cameras, "Cameras", n_cam},
	};
	const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered];
	for (int i = 0; i < 4; ++i) {
		if (i)
			ImGui::SameLine();
		bool active = (m_filter == filters[i].mode);
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, accent);
		char lbl[48];
		snprintf(lbl, sizeof(lbl), "%s %u##filter%d", filters[i].name, filters[i].count, i);
		if (ImGui::SmallButton(lbl))
			m_filter = filters[i].mode;
		if (active)
			ImGui::PopStyleColor();
	}
}

// Flat list of entities matching the active type filter or search
void HierarchyPanel::renderFlatList(Registry& registry, EditorState& state) {
	uint32_t scan = registry.maxEntityIndex();
	for (uint32_t i = 0; i < scan; ++i) {
		if (!registry.isAliveAtIndex(i))
			continue;
		Entity e = registry.entityFromIndex(i);
		if (matchesTypeFilter(registry, e) && matchesNameSearch(registry, e))
			renderEntityNode(registry, e, state, /*flat=*/true);
	}
}

// Lights filter: grouped management view, each group collapsible with its own
// bulk controls
void HierarchyPanel::renderSaveLightsButton(Registry& registry) {
	VeScene* scene = m_scene_manager ? m_scene_manager->getActiveScene() : nullptr;
	const std::filesystem::path path = scene ? scene->sceneOverlayPath() : std::filesystem::path{};

	ImGui::BeginDisabled(path.empty());
	if (ImGui::Button("Save Lights")) {
		if (SceneOverlay::saveEmissiveState(registry, path))
			m_save_lights_status = "Saved " + pathToUtf8(path.filename());
		else
			m_save_lights_status = "Save failed (see log)";
	}
	ImGui::EndDisabled();

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		if (path.empty())
			ImGui::SetTooltip("This scene has no overlay file to save to.");
		else
			ImGui::SetTooltip("Write all enabled lights (color/intensity/spot)\nto %s", pathToUtf8(path).c_str());
	}
	if (!m_save_lights_status.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", m_save_lights_status.c_str());
	}
	ImGui::Separator();
}

void HierarchyPanel::renderLightGroups(Registry& registry, EditorState& state) {
	std::vector<Entity> dir, spot, point, area;
	uint32_t scan = registry.maxEntityIndex();
	for (uint32_t i = 0; i < scan; ++i) {
		if (!registry.isAliveAtIndex(i))
			continue;
		Entity e = registry.entityFromIndex(i);
		if (!matchesNameSearch(registry, e))
			continue;
		if (registry.hasComponent<DirectionalLightComponent>(e)) dir.push_back(e);
		else if (registry.hasComponent<SpotLightComponent>(e)) spot.push_back(e);
		else if (registry.hasComponent<PointLightComponent>(e)) point.push_back(e);
		else if (registry.hasComponent<AreaLightComponent>(e)) area.push_back(e);
	}
	std::sort(point.begin(), point.end());

	auto renderRows = [&](const std::vector<Entity>& es) {
		for (Entity e : es)
			renderEntityNode(registry, e, state, /*flat=*/true);
	};

	if (!dir.empty() && ImGui::TreeNodeEx("Directional", ImGuiTreeNodeFlags_DefaultOpen)) {
		renderEnableCheckbox("Enable all", dir, registry);
		renderRows(dir);
		ImGui::TreePop();
	}

	if (!spot.empty() && ImGui::TreeNodeEx("Spot", ImGuiTreeNodeFlags_DefaultOpen)) {
		renderEnableCheckbox("Enable all", spot, registry);
		renderGroupControls<SpotLightComponent>("all_sl", spot, registry);
		renderRows(spot);
		ImGui::TreePop();
	}

	if (!area.empty() && ImGui::TreeNodeEx("Area", ImGuiTreeNodeFlags_DefaultOpen)) {
		renderEnableCheckbox("Enable all", area, registry);
		renderGroupControls<AreaLightComponent>("all_al", area, registry);
		renderRows(area);
		ImGui::TreePop();
	}

	if (!point.empty() && ImGui::TreeNodeEx("Point", ImGuiTreeNodeFlags_DefaultOpen)) {
		renderEnableCheckbox("Enable all", point, registry);
		renderGroupControls<PointLightComponent>("all_pl", point, registry);

		std::vector<Entity> scene_lights, punctual_lights, emissive_lights;
		for (Entity e : point) {
			switch (registry.getLightSource(e)) {
				case LightSource::Punctual: punctual_lights.push_back(e); break;
				case LightSource::Emissive: emissive_lights.push_back(e); break;
				default:                    scene_lights.push_back(e); break;
			}
		}

		renderRows(scene_lights);

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
				renderGroupControls<PointLightComponent>(group_label, entities, registry);
				renderLightNameGroups(registry, group_label, entities, state);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		ImGui::TreePop();
	}
}

void HierarchyPanel::renderLightNameGroups(Registry& registry, const std::string& source_key,
                                           const std::vector<Entity>& group_lights, EditorState& state) {
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

	auto ungrouped_it = sub_groups.find("Ungrouped");
	if (ungrouped_it != sub_groups.end())
		for (auto e : ungrouped_it->second)
			renderEntityNode(registry, e, state, /*flat=*/true);

	for (auto& [sub_name, sub_lights] : sub_groups) {
		if (sub_name == "Ungrouped") continue;
		ImGui::PushID(sub_name.c_str());
		char sub_header[128];
		snprintf(sub_header, sizeof(sub_header), "%s (%zu)", sub_name.c_str(), sub_lights.size());
		if (ImGui::TreeNode(sub_header)) {
			renderEnableCheckbox("Enable group", sub_lights, registry);
			renderGroupControls<PointLightComponent>(source_key + "/" + sub_name, sub_lights, registry);
			for (auto e : sub_lights)
				renderEntityNode(registry, e, state, /*flat=*/true);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
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

template <typename LightT>
void HierarchyPanel::renderGroupControls(const std::string& key, const std::vector<Entity>& lights, Registry& registry) {
	if (lights.empty()) return;

	auto& state = m_group_states[key];

	if (!state.initialized) {
		if (auto* l = registry.getComponent<LightT>(lights[0])) {
			state.color = l->getColor();
			state.range = l->getRange();
		}
		for (auto e : lights)
			if (auto* l = registry.getComponent<LightT>(e))
				state.base_intensity[e.id()] = l->getIntensity();
		state.initialized = true;
	}

	// Intensity multiplier
	ImGui::Text("Intensity");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(70.0f);
	if (ImGui::DragFloat("##intensity", &state.intensity_multiplier, 0.001f, 0.001f, 10.0f, "%.3fx")) {
		for (auto e : lights) {
			auto* l = registry.getComponent<LightT>(e);
			if (!l) continue;
			auto it = state.base_intensity.find(e.id());
			float base = (it != state.base_intensity.end()) ? it->second : l->getIntensity();
			if (it == state.base_intensity.end())
				state.base_intensity[e.id()] = base;
			l->setIntensity(base * state.intensity_multiplier);
		}
	}

	// Color override
	ImGui::SameLine();
	ImGui::Text("Color");
	ImGui::SameLine();
	if (ImGui::ColorEdit3("##color", glm::value_ptr(state.color),
			ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
		for (auto e : lights) {
			auto* l = registry.getComponent<LightT>(e);
			if (l)
				l->setColor(state.color);
		}
	}

	// Range override
	ImGui::SameLine();
	ImGui::Text("Range");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(70.0f);
	if (ImGui::DragFloat("##range", &state.range, 0.1f, 0.0f, 1000.0f, "%.1f")) {
		for (auto e : lights) {
			auto* l = registry.getComponent<LightT>(e);
			if (l)
				l->setRange(state.range);
		}
	}
}

HierarchyPanel::HierarchyPanel() = default;

HierarchyPanel::~HierarchyPanel() = default;

void HierarchyPanel::setEventBus(EventBus* bus) {
	m_event_bus = bus;
}

void HierarchyPanel::renderSceneSelector() {
	if (!m_scene_manager || !m_event_bus)
		return;

	const auto& entries = m_scene_manager->entries();
	bool loading = m_scene_manager->assetLoader().getState() != LoadState::IDLE
	               && m_scene_manager->assetLoader().getState() != LoadState::FAILED;

	if (loading)
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Loading...");

	int current = m_scene_manager->loadedSceneIndex();
	for (int i = 0; i < static_cast<int>(entries.size()); i++) {
		if (ImGui::RadioButton(entries[static_cast<size_t>(i)].name.c_str(), &current, i)) {
			m_event_bus->emitImmediate(SceneLoadRequestedEvent{.scene_index = i});
			VE_LOGI("Scene load requested: index=" << i << " name='" << entries[static_cast<size_t>(i)].name << "'");
		}
		if (i < static_cast<int>(entries.size()) - 1 && i < 2)
			ImGui::SameLine();
	}

	if (ImGui::Button("New Empty Scene")) {
		m_event_bus->emitImmediate(SceneLoadRequestedEvent{.scene_index = -1});
		VE_LOGI("New empty scene load requested");
	}

	ImGui::Separator();
	ImGui::Spacing();
}

} // namespace ve