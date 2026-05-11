// Editor
//
// ImGui docking editor that owns the built-in panels (viewport, hierarchy, inspector,
// performance, graphics, environment, debug) and drives the per-frame UI render cycle.
// VeApplication creates a single Editor instance and wires it to the renderer, camera,
// and active scene. External systems (physics, shadows, skybox) are injected via setters
// so the editor can expose their state in the appropriate panels.

#pragma once
#include "ve_export.hpp"
#include "ui/editor_state.hpp"
#include "ui/editor_camera_controller.hpp"
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
class InputController;
class VeRenderer;
struct CameraView;
class Registry;
class VeScene;
class SkyboxRenderSystem;
class ShadowRenderSystem;
class PhysicsSystem;
struct UIContext;
struct SceneEntry;
struct SceneLoadRequest;

class EventBus;

class VENGINE_API Editor {
public:
	Editor(VeRenderer& renderer, ImGuiLayer& imgui_layer, EventBus& event_bus);
	~Editor();

	Editor(const Editor&) = delete;
	Editor& operator=(const Editor&) = delete;

	bool beginFrame();
	void renderUI(UIContext& context, Registry* registry, VeScene* active_scene = nullptr);
	void onSwapChainRecreated();

	// State
	EditorState& getState() { return m_state; }
	const EditorState& getState() const { return m_state; }
	bool isEditorMode() const { return m_state.editor_mode; }

	// System injection
	void setAppUICallback(std::function<void()> cb) { m_app_ui_callback = std::move(cb); }
	void setSceneRegistry(const std::vector<SceneEntry>* entries, int* current_index, SceneLoadRequest* request);

	// Per-frame camera view used by gizmo, raycast, and debug overlays.
	// Pointer must remain valid for the lifetime of the editor.
	void setCameraView(const CameraView* camera_view);

	// Skybox system access for environment panel
	void setSkyboxSystem(SkyboxRenderSystem* skybox);

	// Physics system access for collision shape debug rendering
	void setPhysicsSystem(PhysicsSystem* ps);

	// Shadow render system access for debug panel atlas inspection
	void setShadowRenderSystem(ShadowRenderSystem* system);

	// Asset loader for rendering loading panel
	void setAssetLoader(AssetLoadingSystem* loader) { m_asset_loader = loader; m_hierarchy_panel.setAssetLoader(loader); }

	// Input controller for keybindings display
	void setInputController(const InputController* ic) { m_input_controller = ic; }

	// Panel access
	HierarchyPanel& getHierarchyPanel() { return m_hierarchy_panel; }
	ViewportPanel& getViewportPanel() { return m_viewport_panel; }
	InspectorPanel& getInspectorPanel() { return m_inspector_panel; }
	PerformancePanel& getPerformancePanel() { return *m_performance_panel; }

	EditorCameraController& editorCamera() { return m_camera_controller; }
	const EditorCameraController& editorCamera() const { return m_camera_controller; }

private:
	bool handleModeTransition();
	bool handleViewportResize();
	void registerViewportImage();
	void renderMenuBar();
	void renderKeybindingsWindow();

	VeRenderer& m_renderer;
	ImGuiLayer& m_imgui_layer;

	EditorState m_state;
	bool m_was_editor_mode = false;

	EditorCameraController m_camera_controller;

	ViewportPanel m_viewport_panel;
	HierarchyPanel m_hierarchy_panel;
	InspectorPanel m_inspector_panel;
	std::unique_ptr<PerformancePanel> m_performance_panel;
	std::unique_ptr<GraphicsPanel> m_graphics_panel;
	std::unique_ptr<EnvironmentPanel> m_environment_panel;
	std::unique_ptr<DebugPanel> m_debug_panel;
	TextureInspector m_texture_inspector;

	const InputController* m_input_controller = nullptr;
	AssetLoadingSystem* m_asset_loader = nullptr;
	LoadingPanel m_loading_overlay;

	std::function<void()> m_app_ui_callback;
};

} // namespace ve
