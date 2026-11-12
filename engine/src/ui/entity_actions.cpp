#include "pch.hpp"
#include "ui/entity_actions.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include <imgui.h>
#include <algorithm>

namespace ve {

namespace {

constexpr const char* DELETE_CONFIRM_ID = "DeleteConfirm";

// What the hierarchy shows as a folder: children, and nothing of its own to draw or light
bool isGroup(Registry& registry, Entity e) {
	if (registry.firstChild(e).isNull())
		return false;
	if (registry.hasComponent<MeshComponent>(e) || registry.hasComponent<PointLightComponent>(e)
	    || registry.hasComponent<SpotLightComponent>(e) || registry.hasComponent<DirectionalLightComponent>(e)
	    || registry.hasComponent<AreaLightComponent>(e) || registry.hasComponent<CameraComponent>(e)
	    || registry.hasComponent<ParticleEmitterComponent>(e) || registry.hasComponent<SkinComponent>(e))
		return false;
	const auto& skins = registry.skins();
	for (uint32_t i = 0; i < skins.size(); i++)
		for (Entity joint : skins.data()[i].getJointEntities())
			if (joint == e)
				return false;
	return true;
}

Entity rootOf(Registry& registry, Entity e) {
	for (Entity p = registry.getParent(e); !p.isNull(); p = registry.getParent(p))
		e = p;
	return e;
}

// Wrap the selected roots (those with no selected ancestor) in a new empty entity
// placed at their world centroid, preserving each child's world transform.
void groupEntities(Registry& registry, EditorState& state, const std::vector<Entity>& targets) {
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
void ungroupEntity(Registry& registry, EditorState& state, Entity group) {
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

void createEntity(Registry& registry, EditorState& state, const EntityRequest& request, Entity parent) {
	Entity e = Entity::null();
	if (request.action == EntityAction::CreatePointLight) {
		e = registry.createGameObject("Point Light");
		registry.addComponent<PointLightComponent>(e);
	} else if (request.action == EntityAction::CreateSpotLight) {
		e = registry.createGameObject("Spot Light");
		registry.addComponent<SpotLightComponent>(e);
	} else {
		e = registry.createGameObject("Entity");
	}
	if (!parent.isNull())
		registry.setParent(e, parent);
	else if (auto* tc = registry.getComponent<TransformComponent>(e))
		tc->setTranslation(request.position);
	state.selectSingle(e);
}

} // namespace

void entityContextMenuItems(Registry& registry, EditorState& state, Entity clicked) {
	const std::vector<Entity> targets = state.isSelected(clicked) ? state.selected_entities : std::vector<Entity>{clicked};
	const bool many = targets.size() > 1;
	auto request = [&](EntityAction action) {
		state.entity_requests.push_back({.action = action, .targets = targets});
	};
	auto requestClicked = [&](EntityAction action) {
		state.entity_requests.push_back({.action = action, .targets = {clicked}});
	};

	if (ImGui::MenuItem(many ? "Focus Selected" : "Focus", "F"))
		request(EntityAction::Focus);
	const bool has_parent = std::any_of(targets.begin(), targets.end(), [&](Entity e) { return registry.hasParent(e); });
	if (ImGui::MenuItem("Select Parent", nullptr, false, has_parent))
		request(EntityAction::SelectParent);
	if (ImGui::MenuItem("Select Root", nullptr, false, has_parent))
		request(EntityAction::SelectRoot);

	ImGui::Separator();
	if (ImGui::MenuItem(many ? "Duplicate Selected" : "Duplicate", "Ctrl+D"))
		request(EntityAction::Duplicate);
	if (ImGui::MenuItem(many ? "Delete Selected" : "Delete", "Del"))
		request(EntityAction::Delete);

	ImGui::Separator();
	if (ImGui::MenuItem(many ? "Group Selected" : "Group", "Ctrl+G"))
		request(EntityAction::Group);
	if (isGroup(registry, clicked) && ImGui::MenuItem("Ungroup"))
		requestClicked(EntityAction::Ungroup);
	if (ImGui::MenuItem("Create Child Entity"))
		requestClicked(EntityAction::CreateEmpty);

	ImGui::Separator();
	bool any_active = false;
	bool any_inactive = false;
	for (Entity e : targets)
		(registry.isActive(e) ? any_active : any_inactive) = true;
	if (any_inactive && ImGui::MenuItem("Set Active"))
		request(EntityAction::SetActive);
	if (any_active && ImGui::MenuItem("Set Inactive"))
		request(EntityAction::SetInactive);

	const bool is_camera = registry.hasComponent<CameraComponent>(clicked);
	const bool is_character = registry.hasComponent<CharacterControllerComponent>(clicked);
	if (is_camera || is_character)
		ImGui::Separator();
	if (is_camera && ImGui::MenuItem("View Through Camera", nullptr, state.viewport_camera == clicked))
		requestClicked(EntityAction::ViewThrough);
	if (is_character) {
		const bool possessed = state.possessed_entity == clicked;
		if (ImGui::MenuItem(possessed ? "Unpossess" : "Possess"))
			requestClicked(possessed ? EntityAction::Unpossess : EntityAction::Possess);
	}
}

void queueEntityShortcuts(EditorState& state) {
	const ImGuiIO& io = ImGui::GetIO();
	if (io.WantTextInput || state.selected_entities.empty())
		return;
	auto request = [&](EntityAction action) {
		state.entity_requests.push_back({.action = action, .targets = state.selected_entities});
	};
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
		request(EntityAction::Delete);
	// Ctrl+X stays free for cut
	if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false))
		ImGui::OpenPopup(DELETE_CONFIRM_ID);
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
		request(EntityAction::Duplicate);
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false))
		request(EntityAction::Group);
	if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
		request(EntityAction::Focus);
}

void entityDeleteConfirm(Registry& registry, EditorState& state) {
	if (!ImGui::BeginPopup(DELETE_CONFIRM_ID))
		return;
	const std::vector<Entity>& targets = state.selected_entities;
	if (targets.empty()) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	if (targets.size() == 1) {
		const std::string& name = registry.getName(targets.front());
		ImGui::Text("Delete %s?", name.empty() ? "<unnamed>" : name.c_str());
	} else {
		ImGui::Text("Delete %zu entities?", targets.size());
	}
	ImGui::Separator();
	bool confirm = ImGui::MenuItem("Delete", "Enter");
	confirm |= ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
	if (confirm) {
		state.entity_requests.push_back({.action = EntityAction::Delete, .targets = targets});
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void applyEntityRequest(Registry& registry, EditorState& state, const EntityRequest& request) {
	std::vector<Entity> targets;
	for (Entity e : request.targets)
		if (registry.isAlive(e))
			targets.push_back(e);
	const Entity first = targets.empty() ? Entity::null() : targets.front();

	switch (request.action) {
		case EntityAction::Duplicate:
			if (targets.empty())
				break;
			state.selected_entities.clear();
			for (Entity e : topMostRoots(registry, targets))
				state.selected_entities.push_back(registry.cloneEntityRecursive(e));
			state.selection_changed = true;
			break;
		case EntityAction::Delete:
			for (Entity e : targets) {
				state.removeFromSelection(e);
				registry.events().emit(DeleteEntityRequest{e, /*recursive=*/true});
			}
			break;
		case EntityAction::Group:
			groupEntities(registry, state, targets);
			break;
		case EntityAction::Ungroup:
			for (Entity g : targets)
				ungroupEntity(registry, state, g);
			break;
		case EntityAction::SetActive:
		case EntityAction::SetInactive:
			for (Entity e : targets)
				registry.setActive(e, request.action == EntityAction::SetActive);
			break;
		case EntityAction::SelectParent:
		case EntityAction::SelectRoot: {
			std::vector<Entity> picked;
			for (Entity e : targets) {
				Entity p = request.action == EntityAction::SelectRoot ? rootOf(registry, e) : registry.getParent(e);
				if (p.isNull())
					p = e;
				if (std::find(picked.begin(), picked.end(), p) == picked.end())
					picked.push_back(p);
			}
			if (!picked.empty()) {
				state.selected_entities = picked;
				state.selection_changed = true;
			}
			break;
		}
		case EntityAction::ViewThrough:
			if (!first.isNull())
				state.viewport_camera = (state.viewport_camera == first) ? Entity::null() : first;
			break;
		case EntityAction::Possess:
			if (!first.isNull())
				possessEntity(registry, state, first);
			break;
		case EntityAction::Unpossess:
			unpossessEntity(registry, state);
			break;
		case EntityAction::CreateEmpty:
		case EntityAction::CreatePointLight:
		case EntityAction::CreateSpotLight:
			// A parent that died since the request cancels it rather than creating at the origin
			if (request.targets.empty() || !first.isNull())
				createEntity(registry, state, request, first);
			break;
		case EntityAction::Focus:
			break;
	}
}

void possessEntity(Registry& registry, EditorState& state, Entity entity) {
	state.possessed_entity = entity;
	if (state.follow_possessed_camera)
		state.viewport_camera = acquireFollowCamera(registry, entity);
}

void unpossessEntity(Registry& registry, EditorState& state) {
	Entity entity = state.possessed_entity;
	state.possessed_entity = Entity::null();
	if (isFollowCameraOf(registry, state.viewport_camera, entity))
		state.viewport_camera = Entity::null();
}

Entity acquireFollowCamera(Registry& registry, Entity target) {
	Entity cam = Entity::null();
	for (auto [e, fc] : registry.view<FollowCameraComponent>().includeInactive()) {
		if (fc.target == target) {
			cam = e;
			break;
		}
	}
	if (cam.isNull()) {
		const std::string& name = registry.getName(target);
		cam = registry.createFollowCamera(target, (name.empty() ? std::string{"Follow"} : name) + " Camera");
	}
	if (auto* fc = registry.getComponent<FollowCameraComponent>(cam))
		fc->alignBehind();
	return cam;
}

bool isFollowCameraOf(Registry& registry, Entity camera, Entity target) {
	const auto* fc = registry.getComponent<FollowCameraComponent>(camera);
	return fc && fc->target == target;
}

} // namespace ve
