#include "application/ve_application.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_device.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/editor.hpp"
#include "vulkan/ve_buffer.hpp"
#include "input/input_controller.hpp"
#include "input/input_action.hpp"
#include "scene/ve_scene.hpp"
#include "scene/scene_manager.hpp"
#include "utils/ve_log.hpp"

#include "rendering/render_pipeline.hpp"
#include "rendering/render_resources.hpp"
#include "rendering/render_settings.hpp"
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
	: m_ve_window(static_cast<int>(config.window_width), static_cast<int>(config.window_height), config.app_name.c_str()),
	  m_ve_device(m_ve_window),
	  m_resource_manager(m_ve_device, m_event_bus),
	  m_ve_renderer(m_ve_device, m_ve_window, m_resource_manager, m_event_bus),
	  m_input_controller(m_ve_window, m_event_bus),
	  m_config(config) {

	m_render_resources = std::make_unique<RenderResources>(m_ve_device, m_resource_manager, m_event_bus);
	m_render_pipeline = std::make_unique<RenderPipeline>(
		m_ve_device, m_ve_renderer, m_resource_manager, *m_render_resources, m_event_bus, m_config);
	m_scene_manager = std::make_unique<SceneManager>(m_resource_manager, *m_render_resources, m_event_bus);
	initSystems();
	initEditor();
}


VeApplication::~VeApplication() {
	m_event_bus.unsubscribe<InputActionEvent>(m_input_action_sub);
	m_event_bus.unsubscribe<SceneLoadedEvent>(m_scene_loaded_sub);
	m_ve_device.getDevice().waitIdle();
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
		m_editor->editorCamera().tick(m_input_controller.getActions(), m_frame_time);

		{
			ZoneScopedN("Scene Tick");
			m_scene_manager->tick(m_frame_time);
		}

		{
			ZoneScopedN("App Update");
			update();
		}

		VeScene* scene = m_scene_manager->getActiveScene();

		// Physics
		if (scene && m_sim.physics_enabled) {
			ZoneScopedN("Physics Update");
			m_ve_renderer.getProfiler().beginCpuTimer(ProfileTimer::PHYSICS);
			m_physics_system->update(m_frame_time, scene->getRegistry());
			m_ve_renderer.getProfiler().endCpuTimer(ProfileTimer::PHYSICS);
		}

		{
			ZoneScopedN("Flush Events");
			m_event_bus.flushEvents();
		}

		UIContext ui{m_render_pipeline->settings(), m_render_pipeline->stats(), m_sim};

		if (!scene) {
			{
				ZoneScopedN("Editor BeginFrame");
				m_editor->beginFrame();
			}
			if (m_ve_renderer.ensureImageAcquired()) {
				ZoneScopedN("UI");
				m_ve_renderer.beginUIRecording(m_editor->isEditorMode());
				{
					ZoneScopedN("Render UI");
					m_editor->renderUI(ui, nullptr);
				}
			}
			m_ve_renderer.endFrame();
			continue;
		}

		{
			ZoneScopedN("Prepare Frame");
			m_render_pipeline->prepareFrame();
		}
		{
			ZoneScopedN("Editor BeginFrame");
			m_editor->beginFrame();
		}
		Registry* reg = m_scene_manager->getActiveRegistry();
		const CameraView& view = m_editor->resolveCameraView(
			reg, m_ve_renderer.getExtentAspectRatio(),
			glm::radians(m_render_pipeline->settings().fov));

		m_render_pipeline->renderFrame(*scene, view,
			m_editor->getState(), m_frame_time, m_total_time);

		bool editor_mode = m_editor->isEditorMode();
		bool ui_ready = m_ve_renderer.ensureImageAcquired();
		if (ui_ready) {
			ZoneScopedN("UI");
			m_ve_renderer.getProfiler().beginCpuTimer(ProfileTimer::UI);
			m_ve_renderer.beginUIRecording(editor_mode);
			{
				ZoneScopedN("Render UI");
				m_editor->renderUI(ui, &scene->getRegistry());
			}
			m_ve_renderer.getProfiler().endCpuTimer(ProfileTimer::UI);
		}

		m_render_pipeline->finalizeFrameTimings();

		{
			ZoneScopedN("End Frame");
			m_ve_renderer.endFrame();
		}
	}

	m_ve_device.getDevice().waitIdle();
}

const RenderServices& VeApplication::renderServices() const {
	return m_render_pipeline->services();
}

const CameraView& VeApplication::cameraView() const {
	return m_editor->cameraView();
}

// ─── Scene Management (proxies to SceneManager) ──────────────────────────────

void VeApplication::registerScene(std::string name,
                                  std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory) {
	m_scene_manager->registerScene(std::move(name), std::move(factory));
}

void VeApplication::loadDefaultScene(int index) {
	m_scene_manager->loadDefaultScene(index);
}

// ─── System Initialization ───────────────────────────────────────────────────

void VeApplication::initSystems() {
	VE_LOGD("Initialising application systems");

	// Register engine-level input actions
	m_input_controller.registerAction({
		.name = "Toggle Performance UI",
		.key = GLFW_KEY_P,
		.trigger = TriggerType::OnPress,
		.context = InputContext::Always,
		.description = "Toggle performance panel"
	});
	if (m_config.register_default_window_hotkeys) {
		m_input_controller.registerAction({
			.name = "Toggle Borderless",
			.key = GLFW_KEY_F11,
			.trigger = TriggerType::OnPress,
			.context = InputContext::Always,
			.description = "Toggle borderless fullscreen"
		});
	}
	m_input_action_sub = m_event_bus.subscribe<InputActionEvent>([this](const InputActionEvent& e) {
		if (e.name == "Toggle Performance UI") {
			m_editor->getState().show_performance = !m_editor->getState().show_performance;
		} else if (e.name == "Toggle Borderless") {
			auto current = m_ve_window.getWindowMode();
			m_ve_window.setWindowMode(current == VeWindow::WindowMode::Borderless
				? VeWindow::WindowMode::Windowed
				: VeWindow::WindowMode::Borderless);
		}
	});

	// Apply scene-default ambient on activation.
	m_scene_loaded_sub = m_event_bus.subscribe<SceneLoadedEvent>([this](const SceneLoadedEvent& e) {
		if (!e.scene)
			return;
		glm::vec4 ambient = e.scene->getDefaultAmbient();
		auto& settings = m_render_pipeline->settings();
		settings.ambient_light_color = glm::vec3(ambient);
		settings.ambient_light_intensity = ambient.w;
	});

	m_physics_system = std::make_unique<PhysicsSystem>(m_event_bus);
}

void VeApplication::initEditor() {
	VE_LOGD("Initialising UI");
	m_editor = std::make_unique<Editor>(m_ve_window, m_ve_device, m_ve_renderer, m_event_bus, m_config);
	m_editor->setAppUICallback([this]() { renderUI(); });
	m_editor->setContext(EditorContext{
		.scene_manager    = m_scene_manager.get(),
		.physics          = m_physics_system.get(),
		.input_controller = &m_input_controller,
		.event_bus        = &m_event_bus,
		.engine_config    = &m_config,
		.resource_manager = &m_resource_manager,
	}, m_render_pipeline->services());

	auto& editor_cam = m_editor->editorCamera();
	editor_cam.setPosition(glm::vec3{20.0f, 20.0f, 20.0f});
	editor_cam.lookAt(glm::vec3{0.0f, 0.0f, 5.0f});

	auto& settings = m_render_pipeline->settings();
	settings.hdr_enabled = m_ve_renderer.hasHdrSupport() && m_ve_renderer.isHdrEnabled();
	settings.fov = glm::degrees(editor_cam.fov());
	settings.ambient_light_color = glm::vec3(DEFAULT_AMBIENT_LIGHT_COLOR);
	settings.ambient_light_intensity = DEFAULT_AMBIENT_LIGHT_COLOR.w;
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
	std::string title = std::format("{} -- {} mode", m_config.app_name, mode_str);
	glfwSetWindowTitle(m_ve_window.getGLFWwindow(), title.c_str());
}

} // namespace ve
