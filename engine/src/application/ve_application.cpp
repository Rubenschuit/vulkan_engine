#include "application/ve_application.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_device.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/editor.hpp"
#include "vulkan/ve_buffer.hpp"
#include "input/input_controller.hpp"
#include "input/input_action.hpp"
#include "scene/ve_scene.hpp"
#include "scene/gltf_scene.hpp"
#include "utils/ve_log.hpp"

#include "rendering/render_pipeline.hpp"
#include "rendering/render_resources.hpp"
#include "resources/asset_loading_system.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "ve_tracy.hpp"
#include "vulkan/ve_debug_utils.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <format>
#include <cassert>

namespace ve {

VeApplication::VeApplication(const EngineConfig& config)
	: m_ve_window(WIDTH, HEIGHT, APP_NAME),
	  m_ve_device(m_ve_window),
	  m_resource_manager(m_ve_device, m_event_bus),
	  m_ve_renderer(m_ve_device, m_ve_window, m_resource_manager),
	  m_input_controller(m_ve_window),
	  m_config(config) {

	m_render_resources = std::make_unique<RenderResources>(m_ve_device, m_resource_manager, m_config);
	m_render_pipeline = std::make_unique<RenderPipeline>(
		m_ve_device, m_ve_renderer, m_resource_manager, *m_render_resources, m_event_bus,
		m_settings, m_stats, m_config);
	m_asset_loader = std::make_unique<AssetLoadingSystem>(m_resource_manager);
	initSystems();
	initEditor();
}

SceneContext VeApplication::getSceneContext() {
	return m_render_resources->makeSceneContext();
}

ParticleSystem& VeApplication::getParticleSystem() {
	return m_render_pipeline->getParticleSystem();
}

FireworksSystem& VeApplication::getFireworksSystem() {
	return m_render_pipeline->getFireworksSystem();
}

SkyboxRenderSystem& VeApplication::getSkyboxSystem() {
	return m_render_pipeline->getSkyboxRenderSystem();
}

PbrRenderSystem& VeApplication::getPbrSystem() {
	return m_render_pipeline->getPbrRenderSystem();
}

bool VeApplication::isParticlesDeclared() const {
	return m_render_pipeline->isParticlesDeclared();
}

bool VeApplication::isFireworksDeclared() const {
	return m_render_pipeline->isFireworksDeclared();
}

VeApplication::~VeApplication() {
	m_ve_device.getDevice().waitIdle();
	unloadScene();
}

// ─── Main Loop ───────────────────────────────────────────────────────────────

void VeApplication::run() {
	VE_LOGI("VeApplication::run starting. Window=" + std::to_string(m_ve_window.getWidth()) + "x" + std::to_string(m_ve_window.getHeight()));

	setWindowTitle();

	while (!m_ve_window.shouldClose()) {
		m_ve_window.pollEvents();

		// Check for window resize or out-of-date swap chain BEFORE starting frame
		if (m_ve_window.wasWindowResized() || m_ve_renderer.isSwapChainOutOfDate()) {
			VE_LOGD("Swap chain recreation triggered in main loop.");
			m_ve_device.getDevice().waitIdle();
			m_ve_window.resetWindowResizedFlag();
			m_ve_renderer.recreateSwapChain();
			onSwapChainRecreated();
			continue;
		}

		m_ve_renderer.getProfiler().beginCpuTimer(ProfileTimer::FRAME_TOTAL);
		ZoneScopedN("Frame");

		{
			ZoneScopedN("Begin Frame");
			if (!m_ve_renderer.beginFrame())
				continue;
		}

		updateFrameTime();
		m_total_time += m_frame_time;

		// Process input and tick the editor camera controller.
		m_input_controller.processInput(m_frame_time);
		m_editor_panel.visible = m_input_controller.isEditorMode();
		m_editor->getState().editor_mode = m_input_controller.isEditorMode();
		m_editor->editorCamera().tick(m_input_controller.getActions(), m_frame_time);

		// Process pending entity deletions. Resource handles dropped here will
		// be queued on VeResourceManager's rescue queue; their underlying GPU
		// resources are destroyed MAX_FRAMES_IN_FLIGHT frames later on tickFrame.
		if (m_active_scene) {
			auto& registry = m_active_scene->getRegistry();
			if (registry.hasPendingDeletions())
				registry.processPendingDeletions();
		}

		processSceneLoadRequest();
		tickAsyncLoader();

		m_event_bus.flushEvents();

		// App per-frame logic (particle config, etc.)
		update();

		if (!m_active_scene) {
			m_ve_renderer.beginUIRecording(m_editor->isEditorMode());
			m_editor->renderUI(m_ui, nullptr, nullptr);
			m_ve_renderer.endFrame();
			continue;
		}

		// Update scene
		{
			ZoneScopedN("Scene Update");
			m_active_scene->update(m_frame_time);
		}

		// Physics
		if (m_sim.physics_enabled) {
			ZoneScopedN("Physics Update");
			m_ve_renderer.getProfiler().beginCpuTimer(ProfileTimer::PHYSICS);
			m_physics_system->update(m_frame_time, m_active_scene->getRegistry());
			m_ve_renderer.getProfiler().endCpuTimer(ProfileTimer::PHYSICS);
		}

		// Events
		m_event_bus.flushEvents();

		// Render
		m_render_pipeline->prepareFrame();
		if (m_editor->beginFrame())
			recreateResolutionDependentSystems();
		updateCamera(glm::radians(m_settings.fov));

		m_render_pipeline->renderFrame(*m_active_scene, m_current_camera_view,
			m_editor->getState(), m_frame_time, m_total_time);

		bool editor_mode = m_editor->isEditorMode();
		{
			ZoneScopedN("UI");
			m_ve_renderer.beginUIRecording(editor_mode);
			Registry* ui_registry = &m_active_scene->getRegistry();
			m_editor->renderUI(m_ui, ui_registry, m_active_scene.get());
		}

		{
			ZoneScopedN("End Frame");
			m_ve_renderer.endFrame();
		}
	}

	m_ve_device.getDevice().waitIdle();
}

const std::string& VeApplication::getAppSettingsWindowName() const {
	return m_imgui_layer->getAppSettingsWindowName();
}

// ─── Scene Management ────────────────────────────────────────────────────────

void VeApplication::setActiveScene(std::unique_ptr<VeScene> scene) {
	unloadScene();
	m_active_scene = std::move(scene);
	if (m_active_scene) {
		glm::vec4 ambient = m_active_scene->getDefaultAmbient();
		m_settings.ambient_light_color = glm::vec3(ambient);
		m_settings.ambient_light_intensity = ambient.w;

		m_render_pipeline->onSceneLoaded(*m_active_scene);

		m_event_bus.emitImmediate(SceneLoadedEvent{&m_active_scene->getRegistry()});
	}
}

Registry* VeApplication::getActiveRegistry() {
	return m_active_scene ? &m_active_scene->getRegistry() : nullptr;
}

void VeApplication::unloadScene() {
	if (!m_active_scene)
		return;
	// SceneResourceManager subscribes to SceneUnloadedEvent and does a
	// vkDeviceWaitIdle before clearing its caches, so the GPU is idle here.
	m_event_bus.emitImmediate(SceneUnloadedEvent{});
	m_active_scene.reset();
	m_resource_manager.flushPendingUnloads();
}

void VeApplication::registerScene(const std::string& name,
								   std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory) {
	m_scene_entries.push_back({name, std::move(factory), {}, {}});
	if (m_current_scene_index < 0)
		m_current_scene_index = 0;
}

void VeApplication::registerAsyncScene(const std::string& name,
                                       const std::filesystem::path& gltf_path,
                                       std::function<std::unique_ptr<VeScene>(const SceneContext&, std::unique_ptr<VeModel>)> factory,
                                       bool extract_lights, bool flip_tex_coord_v) {
	m_scene_entries.push_back({name, {}, gltf_path, std::move(factory), extract_lights, flip_tex_coord_v});
	if (m_current_scene_index < 0)
		m_current_scene_index = 0;
}

void VeApplication::loadDefaultScene(int index) {
	if (index < 0 || index >= static_cast<int>(m_scene_entries.size()))
		return;
	auto ctx = getSceneContext();
	auto scene = m_scene_entries[static_cast<size_t>(index)].factory(ctx);
	setActiveScene(std::move(scene));
	m_loaded_scene_index = index;
	m_current_scene_index = index;
}

void VeApplication::processSceneLoadRequest() {
	if (m_pending_load.type == SceneLoadRequest::Type::NONE)
		return;

	auto ctx = getSceneContext();
	switch (m_pending_load.type) {
		case SceneLoadRequest::Type::LOAD_REGISTERED: {
			int idx = m_pending_load.scene_index;
			if (idx >= 0 && idx < static_cast<int>(m_scene_entries.size()) && idx != m_loaded_scene_index) {
				const auto& entry = m_scene_entries[static_cast<size_t>(idx)];
				if (!entry.gltf_path.empty() && entry.async_factory) {
					m_asset_loader->beginModelLoad(entry.gltf_path, entry.extract_lights, entry.flip_tex_coord_v);
					m_async_load_type = SceneLoadRequest::Type::LOAD_REGISTERED;
					m_pending_async_scene_index = idx;
				} else {
					m_asset_loader->cancel();
					auto scene = entry.factory(ctx);
					setActiveScene(std::move(scene));
					m_loaded_scene_index = idx;
					m_current_scene_index = idx;
				}
			}
			break;
		}
		case SceneLoadRequest::Type::NEW_EMPTY: {
			m_asset_loader->cancel();
			auto scene = std::make_unique<GltfScene>(ctx);
			setActiveScene(std::move(scene));
			m_loaded_scene_index = -1;
			m_current_scene_index = -1;
			break;
		}
		case SceneLoadRequest::Type::ADD_MODEL: {
			if (m_active_scene) {
				m_asset_loader->beginModelLoad(m_pending_load.gltf_path, true, m_pending_load.flip_tex_coord_v);
				m_async_load_type = SceneLoadRequest::Type::ADD_MODEL;
			}
			break;
		}
		default:
			break;
	}
	m_pending_load.type = SceneLoadRequest::Type::NONE;
}

void VeApplication::tickAsyncLoader() {
	if (m_asset_loader->getState() == LoadState::IDLE ||
	    m_asset_loader->getState() == LoadState::FAILED)
		return;

	m_asset_loader->tick(&*m_render_resources->pool(), &m_render_resources->materialSetLayout());

	if (m_asset_loader->getState() == LoadState::READY)
		finalizeAsyncLoad();
}

void VeApplication::finalizeAsyncLoad() {
	auto model = m_asset_loader->takeModel();
	if (!model)
		return;

	auto ctx = getSceneContext();
	if (m_async_load_type == SceneLoadRequest::Type::LOAD_REGISTERED) {
		int idx = m_pending_async_scene_index;
		if (idx >= 0 && idx < static_cast<int>(m_scene_entries.size()) && m_scene_entries[static_cast<size_t>(idx)].async_factory) {
			auto scene = m_scene_entries[static_cast<size_t>(idx)].async_factory(ctx, std::move(model));
			setActiveScene(std::move(scene));
			m_loaded_scene_index = idx;
			m_current_scene_index = idx;
		}
		m_pending_async_scene_index = -1;
	} else if (m_async_load_type == SceneLoadRequest::Type::ADD_MODEL && m_active_scene) {
		model->addToScene(m_active_scene->getRegistry(),
		                  {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
		m_event_bus.emitImmediate(AssetLoadCompleteEvent{
			m_asset_loader->getModelName(), {}});
	}
	m_async_load_type = SceneLoadRequest::Type::NONE;
	m_editor->getHierarchyPanel().setLoadTimeDisplay(4.f);
}

// ─── Swap Chain Recreation ───────────────────────────────────────────────────

void VeApplication::onSwapChainRecreated() {
	m_ve_device.assertDeviceIdle();
	m_render_pipeline->emitSwapChainRecreatedEvents();
	m_imgui_layer->recreatePipeline();
	m_editor->onSwapChainRecreated();
}

void VeApplication::recreateResolutionDependentSystems() {
	m_render_pipeline->emitResolutionChangedEvent();
}

// ─── System Initialization ───────────────────────────────────────────────────

void VeApplication::initSystems() {
	VE_LOGD("Initialising application systems");

	// Register engine-level input actions
	m_input_controller.setEventBus(&m_event_bus);
	m_input_controller.registerAction({
		.name = "Toggle Performance UI",
		.key = GLFW_KEY_P,
		.trigger = TriggerType::OnPress,
		.context = InputContext::Always,
		.description = "Toggle performance panel"
	});
	m_event_bus.subscribe<InputActionEvent>([this](const InputActionEvent& e) {
		if (e.name == "Toggle Performance UI")
			m_editor->getState().show_performance = !m_editor->getState().show_performance;
	});

	m_physics_system = std::make_unique<PhysicsSystem>();
	m_physics_system->setEventBus(&m_event_bus);
}

void VeApplication::initEditor() {
	VE_LOGD("Initialising UI");
	m_imgui_layer = std::make_unique<ImGuiLayer>(m_ve_window, m_ve_device, m_ve_renderer);
	m_imgui_layer->setAppSettingsWindowName(m_config.app_name);
	m_editor = std::make_unique<Editor>(m_ve_renderer, *m_imgui_layer, m_event_bus);
	m_editor->setAppUICallback([this]() { renderUI(); });

	// Wire scene registry and systems into editor
	m_editor->setSceneRegistry(&m_scene_entries, &m_current_scene_index, &m_pending_load);
	m_editor->setSkyboxSystem(&m_render_pipeline->getSkyboxRenderSystem());
	m_editor->setShadowRenderSystem(&m_render_pipeline->getShadowRenderSystem());
	m_editor->setPhysicsSystem(m_physics_system.get());
	m_editor->setAssetLoader(m_asset_loader.get());
	m_editor->setCameraView(&m_current_camera_view);
	m_editor->setInputController(&m_input_controller);

	auto& editor_cam = m_editor->editorCamera();
	editor_cam.setPosition(glm::vec3{20.0f, 20.0f, 20.0f});
	editor_cam.setFov(m_fov);
	editor_cam.setNearFar(m_near_plane, m_far_plane);
	editor_cam.lookAt(glm::vec3{0.0f, 0.0f, 5.0f});

	m_settings.hdr_enabled = m_ve_renderer.hasHdrSupport() && m_ve_renderer.isHdrEnabled();
	m_settings.fov = glm::degrees(m_fov);
	m_settings.ambient_light_color = glm::vec3(DEFAULT_AMBIENT_LIGHT_COLOR);
	m_settings.ambient_light_intensity = DEFAULT_AMBIENT_LIGHT_COLOR.w;
}

// ─── Camera ──────────────────────────────────────────────────────────────────

void VeApplication::updateCamera(float fov_radians) {
	float aspect = m_ve_renderer.getExtentAspectRatio();
	if (aspect > 0.0f)
		m_last_aspect = aspect;
	m_fov = fov_radians;

	auto& editor_cam = m_editor->editorCamera();
	editor_cam.setFov(m_fov);
	editor_cam.setNearFar(m_near_plane, m_far_plane);

	Entity vp_cam = m_editor->getState().viewport_camera;
	Registry* reg = m_active_scene ? &m_active_scene->getRegistry() : nullptr;
	if (!vp_cam.isNull() && reg && reg->isAlive(vp_cam) && reg->hasComponent<CameraComponent>(vp_cam))
		m_current_camera_view = buildCameraView(*reg, vp_cam, m_last_aspect);
	else
		m_current_camera_view = editor_cam.buildView(m_last_aspect);
}

void VeApplication::updateFrameTime() {
	auto now = clock::now();
	m_frame_time = std::chrono::duration<float, std::chrono::seconds::period>(now - m_last_frame_time).count();
	m_last_frame_time = now;
	const float max_dt = 1.0f / 10.0f;
	if (m_frame_time < 0.0f)
		m_frame_time = 0.0f;
	if (m_frame_time > max_dt)
		m_frame_time = max_dt;
}

void VeApplication::setWindowTitle() {
#ifdef NDEBUG
	const char* mode_str = "Release";
#else
	const char* mode_str = "Debug";
#endif
	std::string title = std::format("Vulkan Engine -- {} mode", mode_str);
	glfwSetWindowTitle(m_ve_window.getGLFWwindow(), title.c_str());
}

} // namespace ve
