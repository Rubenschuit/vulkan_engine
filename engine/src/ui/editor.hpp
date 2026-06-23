// Editor
//
// ImGui docking editor that owns the built-in panels (viewport, hierarchy, inspector,
// performance, graphics, environment, debug) and drives the per-frame UI render cycle.
// VeApplication creates a single Editor instance and wires it to the renderer, camera,
// and active scene. External systems (physics, shadows, skybox) are injected via setters
// so the editor can expose their state in the appropriate panels.

#pragma once
#include "ve_export.hpp"
#include "events/event_bus.hpp"
#include "scene/camera_view.hpp"
#include "ui/editor_context.hpp"
#include "ui/editor_state.hpp"
#include "ui/editor_camera_controller.hpp"
#include "ui/panels/viewport_panel.hpp"
#include "ui/panels/hierarchy_panel.hpp"
#include "ui/panels/inspector_panel.hpp"
#include "ui/panels/performance_panel.hpp"
#include "ui/panels/graphics_panel.hpp"
#include "ui/panels/environment_panel.hpp"
#include "ui/panels/debug_panel.hpp"
#include "ui/panels/asset_browser_panel.hpp"
#include "ui/panels/loading_overlay.hpp"
#include "ui/texture_inspector.hpp"
#include <functional>
#include <memory>

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

namespace ve {

class ImGuiLayer;
class VeWindow;
class VeDevice;
class VeRenderer;
class Registry;
class VeScene;
struct UIContext;
struct RenderServices;
struct EngineConfig;

class VENGINE_API Editor {
public:
	Editor(VeWindow& window, VeDevice& device, VeRenderer& renderer,
	       EventBus& event_bus, const EngineConfig& config);
	~Editor();

	Editor(const Editor&) = delete;
	Editor& operator=(const Editor&) = delete;

	void beginFrame();
	void renderUI(UIContext& context, Registry* registry);

	// Picks between the entity-bound viewport camera and the editor camera, updates
	// the editor camera's FOV, and caches the resolved view.
	const CameraView& resolveCameraView(Registry* registry, float aspect, float fov_y_radians);
	const CameraView& cameraView() const { return m_current_camera_view; }

	// State
	EditorState& getState() { return m_state; }
	const EditorState& getState() const { return m_state; }
	bool isEditorMode() const { return m_state.editor_mode; }

	// Call after construction with all engine references.
	// Pointers must remain valid for the lifetime of the editor.
	void setContext(const EditorContext& ctx, const RenderServices& services);

	// App-specific UI hook (rendered inside the "App Settings" window).
	void setAppUICallback(std::function<void()> cb) { m_app_ui_callback = std::move(cb); }

	// Panel access
	HierarchyPanel& getHierarchyPanel() { return m_hierarchy_panel; }
	ViewportPanel& getViewportPanel() { return m_viewport_panel; }
	InspectorPanel& getInspectorPanel() { return m_inspector_panel; }
	PerformancePanel& getPerformancePanel() { return *m_performance_panel; }

	EditorCameraController& editorCamera() { return m_camera_controller; }
	const EditorCameraController& editorCamera() const { return m_camera_controller; }

private:
	void handleModeTransition();
	void handleViewportResize();
	void registerViewportImage();
	void renderMenuBar();
	void renderKeybindingsWindow();

	VeRenderer& m_renderer;
	std::unique_ptr<ImGuiLayer> m_imgui_layer;

	EditorState m_state;
	bool m_was_editor_mode = false;

	EditorCameraController m_camera_controller;

	ViewportPanel m_viewport_panel;
	HierarchyPanel m_hierarchy_panel;
	InspectorPanel m_inspector_panel;
	AssetBrowserPanel m_asset_browser_panel;
	std::unique_ptr<PerformancePanel> m_performance_panel;
	std::unique_ptr<GraphicsPanel> m_graphics_panel;
	std::unique_ptr<EnvironmentPanel> m_environment_panel;
	std::unique_ptr<DebugPanel> m_debug_panel;
	TextureInspector m_texture_inspector;

	EditorContext m_context{};
	LoadingPanel m_loading_overlay;

	CameraView m_current_camera_view{};
	float m_last_aspect{0.0f};

	std::function<void()> m_app_ui_callback;

	EventBus& m_event_bus;
	EventSubscriptionId m_swap_chain_recreated_sub = 0;
};

} // namespace ve
