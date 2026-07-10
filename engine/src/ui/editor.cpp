#include "pch.hpp"
#include "ui/editor.hpp"
#include "ui/imgui_layer.hpp"
#include "input/input_controller.hpp"
#include "input/input_action.hpp"
#include "rendering/render_services.hpp"
#include "rendering/ve_renderer.hpp"
#include "scene/scene_manager.hpp"
#include "application/ve_application.hpp"
#include "application/ve_engine_config.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include <imgui.h>
#include <cmath>

namespace ve {

Editor::Editor(VeWindow& window, VeDevice& device, VeRenderer& renderer,
               EventBus& event_bus, const EngineConfig& config)
	: m_renderer(renderer), m_event_bus(event_bus) {
	m_imgui_layer = std::make_unique<ImGuiLayer>(window, device, renderer, event_bus);
	m_imgui_layer->setAppSettingsWindowName(config.app_name);
	m_performance_panel = std::make_unique<PerformancePanel>(renderer);
	m_graphics_panel = std::make_unique<GraphicsPanel>(renderer, window, event_bus);
	m_environment_panel = std::make_unique<EnvironmentPanel>();
	m_debug_panel = std::make_unique<DebugPanel>(m_texture_inspector);
	m_inspector_panel.setTextureInspector(&m_texture_inspector);
	registerViewportImage();

	m_swap_chain_recreated_sub = m_event_bus.subscribe<SwapChainRecreatedEvent>(
		[this](const SwapChainRecreatedEvent&) {
			registerViewportImage();
			m_texture_inspector.invalidateCache();
			m_asset_browser_panel.invalidateThumbnails();
		});
}

Editor::~Editor() {
	m_event_bus.unsubscribe<SwapChainRecreatedEvent>(m_swap_chain_recreated_sub);
}

void Editor::beginFrame() {
	if (m_context.input_controller)
		m_state.editor_mode = m_context.input_controller->isEditorMode();
	handleModeTransition();
	if (m_state.editor_mode)
		handleViewportResize();
}

void Editor::handleModeTransition() {
	bool editor_mode = m_state.editor_mode;
	if (!editor_mode && m_was_editor_mode) {
		m_renderer.waitIdle();
		m_renderer.resetSceneRenderExtent();
	}
	m_was_editor_mode = editor_mode;
}

void Editor::handleViewportResize() {
	uint32_t vp_w = static_cast<uint32_t>(m_state.viewport_width);
	uint32_t vp_h = static_cast<uint32_t>(m_state.viewport_height);
	auto cur_extent = m_renderer.getExtent();
	if (vp_w > 0 && vp_h > 0 &&
		(std::abs(static_cast<int>(vp_w) - static_cast<int>(cur_extent.width)) > 4 ||
		 std::abs(static_cast<int>(vp_h) - static_cast<int>(cur_extent.height)) > 4)) {
		m_renderer.waitIdle();
		m_renderer.resizeSceneRender(vp_w, vp_h);
		m_renderer.resizeViewportImage(vp_w, vp_h);
		registerViewportImage();
	}
}

void Editor::registerViewportImage() {
	auto vp_view = m_renderer.getViewportImageView();
	auto vp_sampler = m_renderer.getViewportSampler();
	if (vp_view != VK_NULL_HANDLE && vp_sampler != VK_NULL_HANDLE) {
		m_imgui_layer->registerViewportImage(vp_sampler, vp_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		m_viewport_panel.setTextureID(m_imgui_layer->getViewportTextureId());
	}
}

const CameraView& Editor::resolveCameraView(Registry* registry, float aspect, float fov_y_radians) {
	if (aspect > 0.0f)
		m_last_aspect = aspect;

	m_camera_controller.setFov(fov_y_radians);

	auto scene_cam = tryGetSceneCamera(registry, m_state.viewport_camera, m_last_aspect);
	m_current_camera_view = scene_cam ? *scene_cam : m_camera_controller.buildView(m_last_aspect);
	return m_current_camera_view;
}

void Editor::renderUI(UIContext& context, Registry* registry) {
	bool editor_mode = m_state.editor_mode;

	// Auto-show inspector when an entity is selected
	if (m_state.selection_changed && !m_state.selectedEntity().isNull())
		m_state.show_inspector = true;

	m_imgui_layer->renderUI(context, m_state, [this, registry, editor_mode](UIContext& ctx) {
		if (editor_mode) {
			if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_state.selected_entities.empty())
				m_state.clearSelection();

			renderMenuBar();
			if (m_state.show_keybindings)
				renderKeybindingsWindow();

			if (m_state.show_viewport)
				m_viewport_panel.render(registry, m_state, ctx);
			if (m_state.show_hierarchy)
				m_hierarchy_panel.render(registry, m_state, ctx);
			if (m_state.show_inspector)
				m_inspector_panel.render(registry, m_state, ctx);
			if (m_state.show_asset_browser)
				m_asset_browser_panel.render(registry, m_state, ctx);
			if (m_state.show_performance)
				m_performance_panel->render(registry, m_state, ctx);
			if (m_state.show_debug)
				m_debug_panel->render(registry, m_state, ctx);
			if (m_state.show_settings)
				m_graphics_panel->render(registry, m_state, ctx);
			if (m_state.show_environment)
				m_environment_panel->render(registry, m_state, ctx);
			m_texture_inspector.render();

			if (m_state.show_app_settings && m_app_ui_callback)
				m_app_ui_callback();
		} else {
			m_performance_panel->render(registry, m_state, ctx);
			if (m_app_ui_callback)
				m_app_ui_callback();
		}

		// Loading overlay (renders on top of everything)
		if (m_context.scene_manager) {
			ImVec2 vp_min(0.f, 0.f);
			ImVec2 vp_max(0.f, 0.f);
			if (editor_mode && m_state.show_viewport && m_viewport_panel.isImageValid()) {
				vp_min = m_viewport_panel.getImageMin();
				vp_max = m_viewport_panel.getImageMax();
			}
			m_loading_overlay.render(m_context.scene_manager->assetLoader(), vp_min, vp_max);
		}
	});

	// Cache AABB offset for gizmo/debug shape placement
	if (m_state.selection_changed && registry) {
		m_state.cached_aabb_offset = glm::vec3(0.0f);
		Entity sel = m_state.selectedEntity();
		if (!sel.isNull() && registry->isAlive(sel)) {
			auto* mc = registry->getComponent<MeshComponent>(sel);
			if (mc && mc->hasMesh()) {
				VeMesh::AABB aabb = mc->getMesh()->getLocalAABB();
				m_state.cached_aabb_offset = (aabb.min + aabb.max) * 0.5f;
			} else if (registry->getComponent<TransformComponent>(sel)) {
				const glm::mat4& sel_world = registry->getWorldTransform(sel);
				glm::vec3 wmin(std::numeric_limits<float>::max());
				glm::vec3 wmax(std::numeric_limits<float>::lowest());
				bool found = false;
				std::function<void(Entity)> gather = [&](Entity e) {
					Entity child = registry->firstChild(e);
					while (!child.isNull()) {
						auto* cmc = registry->getComponent<MeshComponent>(child);
						if (cmc && cmc->hasMesh()) {
							VeMesh::AABB la = cmc->getMesh()->getLocalAABB();
							const glm::mat4& cw = registry->getWorldTransform(child);
							for (int c = 0; c < 8; c++) {
								glm::vec3 corner{
									(c & 1) ? la.max.x : la.min.x,
									(c & 2) ? la.max.y : la.min.y,
									(c & 4) ? la.max.z : la.min.z};
								glm::vec3 wp = glm::vec3(cw * glm::vec4(corner, 1.0f));
								wmin = glm::min(wmin, wp);
								wmax = glm::max(wmax, wp);
							}
							found = true;
						}
						gather(child);
						child = registry->nextSibling(child);
					}
				};
				gather(sel);
				if (found) {
					glm::vec3 world_center = (wmin + wmax) * 0.5f;
					m_state.cached_aabb_offset = glm::vec3(
						glm::inverse(sel_world) * glm::vec4(world_center, 1.0f));
				}
			}
		}
	}

	m_state.selection_changed = false;
}

void Editor::setContext(const EditorContext& ctx, const RenderServices& services) {
	m_context = ctx;
	m_hierarchy_panel.setSceneManager(ctx.scene_manager);
	m_hierarchy_panel.setEventBus(ctx.event_bus);
	m_viewport_panel.setCameraView(&m_current_camera_view);
	m_viewport_panel.setPhysicsSystem(ctx.physics);
	m_viewport_panel.setEventBus(ctx.event_bus);
	m_asset_browser_panel.setEventBus(ctx.event_bus);
	m_asset_browser_panel.setResourceManager(ctx.resource_manager);
	m_asset_browser_panel.setCameraView(&m_current_camera_view);
	m_asset_browser_panel.setTextureInspector(&m_texture_inspector);
	if (ctx.engine_config)
		m_asset_browser_panel.setAssetRoot(ctx.engine_config->working_dir);
	m_inspector_panel.setResourceManager(ctx.resource_manager);
	if (m_environment_panel)
		m_environment_panel->setSkyboxSystem(services.skybox);
	if (m_debug_panel)
		m_debug_panel->setShadowRenderSystem(services.shadow);
	if (m_graphics_panel) {
		m_graphics_panel->setParticleBackend(services.particles);
		if (ctx.engine_config)
			m_graphics_panel->setMaxParticleCapacity(ctx.engine_config->max_particle_capacity);
	}
}

void Editor::renderMenuBar() {
	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("View")) {
			ImGui::MenuItem("Viewport", nullptr, &m_state.show_viewport);
			ImGui::MenuItem("Hierarchy", nullptr, &m_state.show_hierarchy);
			ImGui::MenuItem("Inspector", nullptr, &m_state.show_inspector);
			ImGui::MenuItem("Asset Browser", nullptr, &m_state.show_asset_browser);
			ImGui::MenuItem("Performance", nullptr, &m_state.show_performance);
			ImGui::MenuItem("Graphics", nullptr, &m_state.show_settings);
			ImGui::MenuItem("Environment", nullptr, &m_state.show_environment);
			ImGui::MenuItem("Debug", nullptr, &m_state.show_debug);
			if (m_app_ui_callback)
				ImGui::MenuItem("App Settings", nullptr, &m_state.show_app_settings);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help")) {
			ImGui::MenuItem("Keybindings", nullptr, &m_state.show_keybindings);
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}
}

void Editor::renderKeybindingsWindow() {
	ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Keybindings", &m_state.show_keybindings)) {
		ImGui::End();
		return;
	}

	auto tableRow = [](const char* key, const char* desc) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(key);
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(desc);
	};

	if (ImGui::CollapsingHeader("Engine", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::BeginTable("engine_keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Action");
			tableRow("Tab", "Toggle editor mode");
			tableRow("Escape", "Exit editor / close app");
			tableRow("WASD", "Move camera");
			tableRow("Space / C", "Move up / down");
			tableRow("Shift", "Sprint");
			tableRow("Arrow keys", "Look around");
			ImGui::EndTable();
		}
	}

	if (ImGui::CollapsingHeader("Gizmo", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::BeginTable("gizmo_keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Action");
			tableRow("T", "Translate");
			tableRow("R", "Rotate");
			tableRow("E", "Scale");
			tableRow("Delete", "Delete entity");
			ImGui::EndTable();
		}
	}

	if (m_context.input_controller && ImGui::CollapsingHeader("Application", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::BeginTable("app_keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Action");
			ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			for (const auto& action : m_context.input_controller->getRegisteredActions()) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				auto key_name = keyDisplayName(action.binding.key);
				ImGui::TextUnformatted(key_name.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(action.binding.description.c_str());
				ImGui::TableNextColumn();
				const char* ctx = "Always";
				if (action.binding.context == InputContext::GameMode)
					ctx = "Game";
				else if (action.binding.context == InputContext::EditorMode)
					ctx = "Editor";
				ImGui::TextUnformatted(ctx);
			}
			ImGui::EndTable();
		}
	}

	ImGui::End();
}

} // namespace ve
