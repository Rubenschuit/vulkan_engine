#pragma once
#include "ve_export.hpp"
#include "ui/editor_state.hpp"
#include "ui/panels/viewport_panel.hpp"
#include "ui/panels/hierarchy_panel.hpp"
#include "ui/panels/inspector_panel.hpp"
#include "ui/panels/performance_panel.hpp"
#include "ui/panels/graphics_panel.hpp"
#include "ui/panels/environment_panel.hpp"
#include "ui/panels/debug_panel.hpp"
#include "ui/panels/loading_overlay.hpp"
#include "ui/texture_inspector.hpp"
#include <functional>
#include <memory>
#include <vector>

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

namespace ve {

class AssetLoadingSystem;
class ImGuiLayer;
class VeRenderer;
class VeCamera;
class Registry;
class VeScene;
class SkyboxRenderSystem;
class ShadowRenderSystem;
class PhysicsSystem;
struct UIContext;
struct SceneEntry;
struct SceneLoadRequest;

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
	void renderUI(UIContext& context, Registry* registry, VeScene* active_scene = nullptr);

	// Re-register viewport image after swapchain recreation.
	void onSwapChainRecreated();

	// State access
	EditorState& getState() { return m_state; }
	const EditorState& getState() const { return m_state; }
	bool isEditorMode() const { return m_state.editor_mode; }

	// App-specific UI rendered inside the editor dockspace
	void setAppUICallback(std::function<void()> cb) { m_app_ui_callback = std::move(cb); }

	// Scene registry (set by VeApplication after registerScene calls)
	void setSceneRegistry(const std::vector<SceneEntry>* entries, int* current_index, SceneLoadRequest* request);

	// Camera access for gizmo rendering
	void setCamera(VeCamera* camera);

	// Skybox system access for environment panel
	void setSkyboxSystem(SkyboxRenderSystem* skybox);

	// Physics system access for collision shape debug rendering
	void setPhysicsSystem(PhysicsSystem* ps);

	// Shadow render system access for debug panel atlas inspection
	void setShadowRenderSystem(ShadowRenderSystem* system);

	// Asset loader for rendering loading panel
	void setAssetLoader(AssetLoadingSystem* loader) { m_asset_loader = loader; m_hierarchy_panel.setAssetLoader(loader); }

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
	std::unique_ptr<GraphicsPanel> m_graphics_panel;
	std::unique_ptr<EnvironmentPanel> m_environment_panel;
	std::unique_ptr<DebugPanel> m_debug_panel;
	TextureInspector m_texture_inspector;

	// Loading UI
	AssetLoadingSystem* m_asset_loader = nullptr;
	LoadingPanel m_loading_overlay;

	// App callback
	std::function<void()> m_app_ui_callback;
};

} // namespace ve
