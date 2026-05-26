#pragma once
#include "events/event_bus.hpp"
#include "ui/editor_panel.hpp"
#include "ui/editor_state.hpp"
#include <filesystem>
#include <glm/vec3.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ve {

class SceneManager;

class VENGINE_API HierarchyPanel : public EditorPanel {
public:
	HierarchyPanel();
	~HierarchyPanel();

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Scene Hierarchy"; }

	void setSceneManager(SceneManager* sm) { m_scene_manager = sm; }
	void setEventBus(EventBus* bus);

private:
	void renderSceneSelector();
	void renderEntityNode(Registry& registry, Entity entity, EditorState& state);
	void renderLightsSection(Registry& registry, EditorState& state);
	void renderLightGroup(Registry& registry, const std::string& source_key, const std::vector<Entity>& group_lights, EditorState& state);
	void renderSelectableLight(Registry& registry, Entity entity, EditorState& state);
	void renderGroupControls(const std::string& key, const std::vector<Entity>& lights, Registry& registry);
	void renderEnableCheckbox(const char* label, const std::vector<Entity>& lights, Registry& registry);

	bool isLightOnly(Registry& registry, Entity entity);
	bool subtreeMatchesSearch(Registry& registry, Entity entity);

	struct LightGroupState {
		float intensity_multiplier = 1.0f;
		glm::vec3 color{1.0f};
		float range = 0.0f;
		bool initialized = false;
	};

	SceneManager* m_scene_manager = nullptr;
	EventBus* m_event_bus = nullptr;

	bool m_flip_tex_coord_v = false;

	std::unordered_map<std::string, LightGroupState> m_group_states;
	Registry* m_last_registry = nullptr;
	Entity m_pending_delete = Entity::null();
	Entity m_pending_duplicate = Entity::null();

	// Auto-expand and scroll-to for selection changes
	std::unordered_set<uint32_t> m_force_open_entities;
	bool m_scroll_to_selected = false;
	bool m_show_lights_in_tree = true;

	std::unordered_set<uint32_t> m_joint_entity_ids;

	// Search
	char m_search_buf[256]{};
	bool m_search_active = false;
};

} // namespace ve