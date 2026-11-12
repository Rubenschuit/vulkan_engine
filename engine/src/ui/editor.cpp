#include "pch.hpp"
#include "ui/editor.hpp"
#include "scene/camera_manager.hpp"
#include "ui/entity_actions.hpp"
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
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ve {

namespace {

constexpr float MIN_FOCUS_RADIUS = 0.5f;

}

Editor::Editor(VeWindow& window, VeDevice& device, VeRenderer& renderer,
               EventBus& event_bus, const EngineConfig& config, CameraManager& cameras)
	: m_renderer(renderer), m_cameras(cameras), m_event_bus(event_bus) {
	m_imgui_layer = std::make_unique<ImGuiLayer>(window, device, renderer, event_bus);
	m_imgui_layer->setAppSettingsWindowName(config.app_name);
	m_viewport_panel.setFlyCamera(&m_cameras.flyCamera());
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
	if (m_context.input_controller) {
		m_state.editor_mode = m_context.input_controller->isEditorMode();
		m_state.mouse_look_active = m_context.input_controller->isMouseLook();
	}
	// The GLFW backend keeps reporting the locked cursor's position, which would hover whatever
	// panel it passes over
	ImGuiIO& io = ImGui::GetIO();
	if (m_state.mouse_look_active)
		io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
	else
		io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
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

void Editor::renderUI(UIContext& context, Registry* registry) {
	bool editor_mode = m_state.editor_mode;

	// Applied ahead of the panels, so the hierarchy expands to and scrolls to what they select
	if (registry) {
		applyEntityRequests(*registry);
	} else {
		m_state.entity_requests.clear();
		m_pending_focus.clear();
	}

	// Auto-show inspector when an entity is selected
	if (m_state.selection_changed && !m_state.selectedEntity().isNull())
		m_state.show_inspector = true;

	m_state.viewport_hovered = false;
	m_state.viewport_focused = false;
	m_state.hierarchy_focused = false;

	m_imgui_layer->renderUI(context, m_state, [this, registry, editor_mode](UIContext& ctx) {
		if (editor_mode) {
			if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_state.selected_entities.empty() && !m_ui_popup_open
			    && !ImGui::GetIO().WantTextInput)
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

			if (registry) {
				// Keys belong to the camera during mouse look
				if (!m_ui_popup_open && !m_state.mouse_look_active
				    && (m_state.viewport_hovered || m_state.viewport_focused || m_state.hierarchy_focused))
					queueEntityShortcuts(m_state);
				entityDeleteConfirm(*registry, m_state);
			}
		} else {
			m_performance_panel->render(registry, m_state, ctx);
			if (m_app_ui_callback)
				m_app_ui_callback();
		}
		m_ui_popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

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

	if (m_context.input_controller) {
		// Escape clears a selection before it leaves the editor
		const bool clears_selection = editor_mode && !m_state.selected_entities.empty();
		m_context.input_controller->setUiKeyboardState(ImGui::GetIO().WantTextInput, m_ui_popup_open || clears_selection);
		if (m_state.mouse_look_requested && editor_mode)
			m_context.input_controller->setMouseLook(true);
	}
	m_state.mouse_look_requested = false;

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

void Editor::applyEntityRequests(Registry& registry) {
	for (const EntityRequest& request : std::exchange(m_state.entity_requests, {})) {
		if (request.action == EntityAction::Focus)
			m_pending_focus = request.targets;
		else
			applyEntityRequest(registry, m_state, request);
	}
	if (m_pending_focus.empty())
		return;

	// Focusing moves the editor camera. Leaving a scene camera reseeds the fly camera from that
	// camera's last view on the next resolve, so the move waits until the fly camera renders.
	if (!m_state.viewport_camera.isNull()) {
		m_state.viewport_camera = Entity::null();
	} else if (m_cameras.currentView().source.isNull()) {
		focusEntities(registry, m_pending_focus);
		m_pending_focus.clear();
	}
}

// Back the editor camera off along its view direction until the targets' bounds fit the view
void Editor::focusEntities(Registry& registry, const std::vector<Entity>& targets) {
	glm::vec3 lo(std::numeric_limits<float>::max());
	glm::vec3 hi(std::numeric_limits<float>::lowest());
	bool found = false;
	auto include = [&](const glm::vec3& a, const glm::vec3& b) {
		lo = glm::min(lo, a);
		hi = glm::max(hi, b);
		found = true;
	};
	for (Entity target : targets) {
		if (!registry.isAlive(target))
			continue;
		bool has_mesh = false;
		std::vector<Entity> stack{target};
		while (!stack.empty()) {
			Entity e = stack.back();
			stack.pop_back();
			auto* mc = registry.getComponent<MeshComponent>(e);
			if (mc && mc->hasMesh()) {
				const VeMesh::AABB& box = mc->getWorldAABB();
				include(box.min, box.max);
				has_mesh = true;
			}
			for (Entity c = registry.firstChild(e); !c.isNull(); c = registry.nextSibling(c))
				stack.push_back(c);
		}
		// Lights, cameras and empty groups count as a point
		if (!has_mesh && registry.getComponent<TransformComponent>(target)) {
			const glm::vec3 p(registry.getWorldTransform(target)[3]);
			include(p, p);
		}
	}
	if (!found)
		return;

	const glm::vec3 center = 0.5f * (lo + hi);
	const float radius = std::max(0.5f * glm::length(hi - lo), MIN_FOCUS_RADIUS);
	const CameraView& view = m_cameras.currentView();
	const float half_fov_y = 0.5f * view.fov_y_radians;
	const float half_fov = std::min(half_fov_y, std::atan(std::tan(half_fov_y) * view.aspect));
	FlyCameraController& fly = m_cameras.flyCamera();
	fly.setPosition(center - fly.forward() * (radius / std::sin(half_fov)));
}

void Editor::setContext(const EditorContext& ctx, const RenderServices& services) {
	m_context = ctx;
	m_hierarchy_panel.setSceneManager(ctx.scene_manager);
	m_hierarchy_panel.setEventBus(ctx.event_bus);
	m_viewport_panel.setCameraView(&m_cameras.currentView());
	m_viewport_panel.setPhysicsSystem(ctx.physics);
	m_viewport_panel.setEventBus(ctx.event_bus);
	m_asset_browser_panel.setEventBus(ctx.event_bus);
	m_asset_browser_panel.setResourceManager(ctx.resource_manager);
	m_asset_browser_panel.setCameraView(&m_cameras.currentView());
	m_asset_browser_panel.setTextureInspector(&m_texture_inspector);
	if (ctx.engine_config)
		m_asset_browser_panel.setAssetRoot(ctx.engine_config->working_dir);
	m_inspector_panel.setResourceManager(ctx.resource_manager);
	if (m_environment_panel)
		m_environment_panel->setSkyboxSystem(services.skybox);
	if (m_debug_panel) {
		m_debug_panel->setShadowRenderSystem(services.shadow);
		m_debug_panel->setFlyCamera(&m_cameras.flyCamera());
		m_debug_panel->setRcSystem(services.rc);
		m_debug_panel->setRayTracingScene(services.ray_tracing);
	}
	if (m_graphics_panel) {
		m_graphics_panel->setParticleBackend(services.particles);
		m_graphics_panel->setRayQuerySupported(services.ray_tracing != nullptr);
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
			tableRow("Escape", "Clear selection, else exit editor / close app");
			tableRow("WASD", "Move camera");
			tableRow("Space / C", "Move up / down");
			tableRow("Shift", "Sprint");
			tableRow("Arrow keys", "Look around");
			tableRow("Right drag", "Look around (viewport)");
			tableRow("Scroll", "Camera speed, while right-dragging");
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
			ImGui::EndTable();
		}
	}

	if (ImGui::CollapsingHeader("Selection (viewport, hierarchy)", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::BeginTable("selection_keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Action");
			tableRow("Right click", "Selection menu (Create Here if none)");
			tableRow("F", "Focus selection");
			tableRow("Ctrl+D", "Duplicate");
			tableRow("Ctrl+G", "Group");
			tableRow("Del", "Delete");
			tableRow("X", "Delete, after a confirm");
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
