#pragma once
#include "ve_export.hpp"
#include "ui/editor_state.hpp"
#include "ui/panels/viewport_panel.hpp"
#include "ui/panels/hierarchy_panel.hpp"
#include "ui/panels/inspector_panel.hpp"
#include "ui/panels/performance_panel.hpp"
#include <functional>
#include <memory>

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

namespace ve {

class ImGuiLayer;
class VeRenderer;
class Registry;
struct UIContext;

class VENGINE_API Editor {
public:
	Editor(VeRenderer& renderer, ImGuiLayer& imgui_layer);
	~Editor();

	Editor(const Editor&) = delete;
	Editor& operator=(const Editor&) = delete;

	// Call before scene rendering. Handles editor mode transitions and viewport resize.
	// Returns true if the scene render extent changed (app should recreate resolution-dependent systems).
	bool beginFrame();

	// Full UI render cycle: dockspace, built-in panels, app callback, selection logic.
	void renderUI(UIContext& context, Registry* registry);

	// Re-register viewport image after swapchain recreation.
	void onSwapChainRecreated();

	// State access
	EditorState& getState() { return m_state; }
	const EditorState& getState() const { return m_state; }
	bool isEditorMode() const { return m_state.editor_mode; }

	// App-specific UI rendered inside the editor dockspace
	void setAppUICallback(std::function<void()> cb) { m_app_ui_callback = std::move(cb); }

	// Panel access for app-side customization
	HierarchyPanel& getHierarchyPanel() { return m_hierarchy_panel; }
	ViewportPanel& getViewportPanel() { return m_viewport_panel; }
	InspectorPanel& getInspectorPanel() { return m_inspector_panel; }
	PerformancePanel& getPerformancePanel() { return *m_performance_panel; }

private:
	bool handleModeTransition();
	bool handleViewportResize();
	void registerViewportImage();

	VeRenderer& m_renderer;
	ImGuiLayer& m_imgui_layer;

	EditorState m_state;
	bool m_was_editor_mode = false;

	// Built-in panels
	ViewportPanel m_viewport_panel;
	HierarchyPanel m_hierarchy_panel;
	InspectorPanel m_inspector_panel;
	std::unique_ptr<PerformancePanel> m_performance_panel;

	// App callback
	std::function<void()> m_app_ui_callback;
};

} // namespace ve
