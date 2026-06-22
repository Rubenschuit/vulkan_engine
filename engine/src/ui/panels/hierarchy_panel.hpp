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

struct ImGuiMultiSelectIO;

namespace ve {

class SceneManager;

enum class TreeFilter { All, Meshes, Lights, Cameras };

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
	void renderEntityNode(Registry& registry, Entity entity, EditorState& state, bool flat = false);
	void renderFilters(Registry& registry);
	void renderFlatList(Registry& registry, EditorState& state);
	void renderLightGroups(Registry& registry, EditorState& state);
	void renderLightNameGroups(Registry& registry, const std::string& source_key, const std::vector<Entity>& group_lights, EditorState& state);
	void renderVisibilityToggle(Registry& registry, Entity entity, bool row_hovered);
	void renderGroupControls(const std::string& key, const std::vector<Entity>& lights, Registry& registry);
	void renderEnableCheckbox(const char* label, const std::vector<Entity>& lights, Registry& registry);
	void applySelectionRequests(ImGuiMultiSelectIO* ms, Registry& registry, EditorState& state);
	void groupEntities(Registry& registry, EditorState& state, const std::vector<Entity>& targets);
	void ungroupEntity(Registry& registry, EditorState& state, Entity group);

	bool matchesTypeFilter(Registry& registry, Entity entity);
	bool matchesNameSearch(Registry& registry, Entity entity);
	bool subtreeVisible(Registry& registry, Entity entity);

	struct LightGroupState {
		float intensity_multiplier = 1.0f;
		glm::vec3 color{1.0f};
		float range = 0.0f;
		bool initialized = false;
		std::unordered_map<uint32_t, float> base_intensity;
	};

	SceneManager* m_scene_manager = nullptr;
	EventBus* m_event_bus = nullptr;

	bool m_flip_tex_coord_v = false;

	std::unordered_map<std::string, LightGroupState> m_group_states;
	Registry* m_last_registry = nullptr;
	std::vector<Entity> m_pending_deletes;
	std::vector<Entity> m_pending_duplicates;
	std::vector<Entity> m_pending_group;
	std::vector<Entity> m_pending_ungroup;

	// Auto-expand and scroll-to for selection changes
	std::unordered_set<uint32_t> m_force_open_entities;
	bool m_scroll_to_selected = false;

	TreeFilter m_filter = TreeFilter::All;

	std::unordered_set<uint32_t> m_joint_entity_ids;

	// Multi-select
	std::vector<Entity> m_visible_order;
	int m_visible_row_index = 0;

	// Inline rename
	Entity m_renaming_entity = Entity::null();
	char m_rename_buf[256]{};
	bool m_rename_focus = false;

	// Search
	char m_search_buf[256]{};
	bool m_search_active = false;
};

} // namespace ve