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
	m_scene_manager = std::make_unique<SceneManager>(m_resource_manager, *m_render_resources, m_event_bus);
	initSystems();
	initEditor();
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

		{
			ZoneScopedN("Scene Tick");
			m_scene_manager->tick(m_frame_time);
		}
		m_event_bus.flushEvents();

		// App per-frame logic (particle config, etc.)
		update();

		VeScene* scene = m_scene_manager->getActiveScene();
		if (!scene) {
			m_ve_renderer.beginUIRecording(m_editor->isEditorMode());
			m_editor->renderUI(m_ui, nullptr, nullptr);
			m_ve_renderer.endFrame();
			continue;
		}

		// Physics
		if (m_sim.physics_enabled) {
			ZoneScopedN("Physics Update");
			m_ve_renderer.getProfiler().beginCpuTimer(ProfileTimer::PHYSICS);
			m_physics_system->update(m_frame_time, scene->getRegistry());
			m_ve_renderer.getProfiler().endCpuTimer(ProfileTimer::PHYSICS);
		}

		// Events
		m_event_bus.flushEvents();

		// Render
		m_render_pipeline->prepareFrame();
		if (m_editor->beginFrame())
			recreateResolutionDependentSystems();
		updateCamera(glm::radians(m_settings.fov));

		m_render_pipeline->renderFrame(*scene, m_current_camera_view,
			m_editor->getState(), m_frame_time, m_total_time);

		bool editor_mode = m_editor->isEditorMode();
		{
			ZoneScopedN("UI");
			m_ve_renderer.beginUIRecording(editor_mode);
			m_editor->renderUI(m_ui, &scene->getRegistry(), scene);
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

// ─── Scene Management (proxies to SceneManager) ──────────────────────────────

void VeApplication::registerScene(std::string name,
                                  std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory) {
	m_scene_manager->registerScene(std::move(name), std::move(factory));
}

void VeApplication::registerGltfScene(std::string name,
                                      std::filesystem::path gltf_path,
                                      std::function<std::unique_ptr<VeScene>(const SceneContext&, std::unique_ptr<VeModel>)> factory,
                                      bool extract_lights, bool flip_tex_coord_v) {
	m_scene_manager->registerGltfScene(std::move(name), std::move(gltf_path), std::move(factory),
	                                   extract_lights, flip_tex_coord_v);
}

void VeApplication::loadDefaultScene(int index) {
	m_scene_manager->loadDefaultScene(index);
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

	// Apply scene-default ambient on activation.
	m_event_bus.subscribe<SceneLoadedEvent>([this](const SceneLoadedEvent& e) {
		if (!e.scene)
			return;
		glm::vec4 ambient = e.scene->getDefaultAmbient();
		m_settings.ambient_light_color = glm::vec3(ambient);
		m_settings.ambient_light_intensity = ambient.w;
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
	m_editor->setContext({
		.scene_manager    = m_scene_manager.get(),
		.skybox           = &m_render_pipeline->getSkyboxRenderSystem(),
		.shadow           = &m_render_pipeline->getShadowRenderSystem(),
		.physics          = m_physics_system.get(),
		.camera_view      = &m_current_camera_view,
		.input_controller = &m_input_controller,
		.event_bus        = &m_event_bus,
	});

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
	VeScene* scene = m_scene_manager->getActiveScene();
	Registry* reg = scene ? &scene->getRegistry() : nullptr;
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
