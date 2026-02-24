#include "pch.hpp"
#include "ui/editor.hpp"
#include "ui/imgui_layer.hpp"
#include "rendering/ve_renderer.hpp"
#include "application/ve_application.hpp"
#include <imgui.h>
#include <cmath>

namespace ve {

Editor::Editor(VeRenderer& renderer, ImGuiLayer& imgui_layer)
	: m_renderer(renderer), m_imgui_layer(imgui_layer) {
	m_performance_panel = std::make_unique<PerformancePanel>(renderer);
	m_graphics_panel = std::make_unique<GraphicsPanel>(renderer);
	m_environment_panel = std::make_unique<EnvironmentPanel>();
	registerViewportImage();
}

Editor::~Editor() = default;

bool Editor::beginFrame() {
	bool resized = handleModeTransition();
	if (m_state.editor_mode)
		resized |= handleViewportResize();
	return resized;
}

bool Editor::handleModeTransition() {
	bool editor_mode = m_state.editor_mode;
	bool resized = false;
	if (!editor_mode && m_was_editor_mode) {
		m_renderer.resetSceneRenderExtent();
		resized = true;
	}
	m_was_editor_mode = editor_mode;
	return resized;
}

bool Editor::handleViewportResize() {
	uint32_t vp_w = static_cast<uint32_t>(m_state.viewport_width);
	uint32_t vp_h = static_cast<uint32_t>(m_state.viewport_height);
	auto cur_extent = m_renderer.getExtent();
	if (vp_w > 0 && vp_h > 0 &&
		(std::abs(static_cast<int>(vp_w) - static_cast<int>(cur_extent.width)) > 4 ||
		 std::abs(static_cast<int>(vp_h) - static_cast<int>(cur_extent.height)) > 4)) {
		m_renderer.resizeSceneRender(vp_w, vp_h);
		m_renderer.resizeViewportImage(vp_w, vp_h);
		registerViewportImage();
		return true;
	}
	return false;
}

void Editor::registerViewportImage() {
	auto vp_view = m_renderer.getViewportImageView();
	auto vp_sampler = m_renderer.getViewportSampler();
	if (vp_view != VK_NULL_HANDLE && vp_sampler != VK_NULL_HANDLE) {
		m_imgui_layer.registerViewportImage(vp_sampler, vp_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		m_viewport_panel.setTextureID(m_imgui_layer.getViewportTextureId());
	}
}

void Editor::renderUI(UIContext& context, Registry* registry, VeScene* active_scene) {
	bool editor_mode = m_state.editor_mode;

	// Auto-show inspector when an entity is selected
	if (m_state.selection_changed && !m_state.selected_entity.isNull())
		m_state.show_inspector = true;

	// Update per-frame state on panels
	m_hierarchy_panel.setActiveScene(active_scene);

	m_imgui_layer.renderUI(context, m_state, [this, registry, editor_mode](UIContext& ctx) {
		if (editor_mode) {
			if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_state.selected_entity.isNull()) {
				m_state.selected_entity = Entity::null();
				m_state.selection_changed = true;
			}

			m_viewport_panel.render(registry, m_state, ctx);
			m_hierarchy_panel.render(registry, m_state, ctx);
			m_inspector_panel.render(registry, m_state, ctx);
			m_performance_panel->render(registry, m_state, ctx);
			m_graphics_panel->render(registry, m_state, ctx);
			m_environment_panel->render(registry, m_state, ctx);

			if (m_app_ui_callback)
				m_app_ui_callback();
		} else {
			m_performance_panel->render(registry, m_state, ctx);
		}
	});

	m_state.selection_changed = false;
}

void Editor::setSceneRegistry(const std::vector<SceneEntry>* entries, int* current_index, SceneLoadRequest* request) {
	m_hierarchy_panel.setSceneRegistry(entries, current_index, request);
}

void Editor::setCamera(VeCamera* camera) {
	m_viewport_panel.setCamera(camera);
}

void Editor::setSkyboxSystem(SkyboxRenderSystem* skybox) {
	if (m_environment_panel)
		m_environment_panel->setSkyboxSystem(skybox);
}

void Editor::onSwapChainRecreated() {
	registerViewportImage();
}

} // namespace ve
