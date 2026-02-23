#pragma once
#include "ui/editor_panel.hpp"
#include "ui/editor_state.hpp"
#include <functional>
#include <glm/vec3.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

class VENGINE_API HierarchyPanel : public EditorPanel {
public:
	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Scene Hierarchy"; }

	void setHeaderCallback(std::function<void()> callback) { m_header_callback = std::move(callback); }

private:
	void renderEntityNode(Registry& registry, Entity entity, EditorState& state);
	void renderLightsSection(Registry& registry, EditorState& state);
	void renderLightGroup(Registry& registry, const std::string& source_key, const std::vector<Entity>& group_lights, EditorState& state);
	void renderSelectableLight(Registry& registry, Entity entity, EditorState& state);
	void renderGroupControls(const std::string& key, const std::vector<Entity>& lights, Registry& registry);
	void renderEnableCheckbox(const char* label, const std::vector<Entity>& lights, Registry& registry);

	bool isLightOnly(Registry& registry, Entity entity);

	struct LightGroupState {
		float intensity_multiplier = 1.0f;
		glm::vec3 color{1.0f};
		float range = 0.0f;
		bool initialized = false;
	};

	std::function<void()> m_header_callback;
	std::unordered_map<std::string, LightGroupState> m_group_states;
	Registry* m_last_registry = nullptr;
	Entity m_pending_delete = Entity::null();
};

} // namespace ve
