#include "sandbox.hpp"
#include "application/ve_entry_point.hpp" // defines main() and createApp(), must be in exactly one TU

#include <imgui.h>

namespace ve {

static EngineConfig makeEngineConfig(const std::filesystem::path& root) {
	return EngineConfig{
		.app_name                = "Sandbox",
		.working_dir             = root,
		.shaders_dir             = root / "shaders",
		.skybox_dir              = root / "textures" / "skybox",
		.light_billboard_texture = root / "textures" / "light.ktx2",
	};
}

Sandbox::Sandbox(const std::filesystem::path& working_dir)
	: VeApplication(makeEngineConfig(working_dir)),
	  m_paths(working_dir) {

	registerScene("Simple", [this](const SceneContext& ctx) {
		return std::make_unique<SimpleScene>(ctx, m_paths);
	});
	registerScene("Sponza", [this](const SceneContext& ctx) {
		return std::make_unique<SponzaScene>(ctx, m_paths);
	});
	registerScene("Bistro", [this](const SceneContext& ctx) {
		return std::make_unique<BistroScene>(ctx, m_paths);
	});

	m_fireworks = std::make_unique<effects::Fireworks>(
		*renderServices().particles, resourceManager(), sceneManager(), eventBus(),
		m_paths.fire_texture, m_paths.smoke_texture);

	m_scene_loaded_sub = eventBus().subscribe<SceneLoadedEvent>(
		[this](const SceneLoadedEvent& e) {
			m_flashlight = Entity::null();
			if (e.registry)
				createFlashlight(*e.registry);
		});

	loadDefaultScene(0);

	registerInputActions();
}

Sandbox::~Sandbox() {
	eventBus().unsubscribe<InputActionEvent>(m_input_sub);
	eventBus().unsubscribe<SceneLoadedEvent>(m_scene_loaded_sub);
}

// ─── Input Actions ───────────────────────────────────────────────────────────

void Sandbox::registerInputActions() {
	auto& ic = getInputController();

	ic.registerAction({
		.name = "Launch Firework", .key = GLFW_KEY_F,
		.trigger = TriggerType::OnPress, .context = InputContext::GameMode,
		.description = "Launch a firework rocket"
	});

	ic.registerAction({
		.name = "Toggle Controls", .key = GLFW_KEY_H,
		.trigger = TriggerType::OnPress, .context = InputContext::Always,
		.description = "Toggle controls overlay"
	});

	ic.registerAction({
		.name = "Toggle Fireworks Panel", .key = GLFW_KEY_K,
		.trigger = TriggerType::OnPress, .context = InputContext::Always,
		.description = "Toggle fireworks parameter panel"
	});

	ic.registerAction({
		.name = "Toggle Flashlight", .key = GLFW_KEY_L,
		.trigger = TriggerType::OnPress, .context = InputContext::Always,
		.description = "Toggle camera-mounted flashlight"
	});

	m_input_sub = eventBus().subscribe<InputActionEvent>(
		[this](const InputActionEvent& e) {
			if (e.name == "Launch Firework" && m_fireworks)
				m_fireworks->launchRocket();
			else if (e.name == "Toggle Controls")
				m_show_controls = !m_show_controls;
			else if (e.name == "Toggle Fireworks Panel")
				m_show_fireworks_panel = !m_show_fireworks_panel;
			else if (e.name == "Toggle Flashlight") {
				m_flashlight_on = !m_flashlight_on;
				Registry* reg = sceneManager().getActiveRegistry();
				if (reg && !m_flashlight.isNull() && reg->isAlive(m_flashlight))
					reg->setActive(m_flashlight, m_flashlight_on);
			}
		});
}

// ─── Per-Frame Update ────────────────────────────────────────────────────────

void Sandbox::update() {
	if (m_fireworks)
		m_fireworks->update(frameTime(), totalTime());

	updateFlashlight();
}

// ─── Flashlight ──────────────────────────────────────────────────────────────

void Sandbox::createFlashlight(Registry& registry) {
	m_flashlight = registry.createSpotLight(
		/*intensity*/ 80.0f,
		/*radius*/    1.0f,
		/*color*/     glm::vec3(1.0f, 0.97f, 0.9f),
		/*direction*/ glm::vec3(0.0f, 0.0f, -1.0f),
		/*inner*/     glm::radians(12.0f),
		/*outer*/     glm::radians(22.0f));
	registry.setName(m_flashlight, "Flashlight");
	registry.getComponent<SpotLightComponent>(m_flashlight)->setShowBillboard(false);
	registry.setActive(m_flashlight, m_flashlight_on);
}

void Sandbox::updateFlashlight() {
	if (!m_flashlight_on || m_flashlight.isNull())
		return;
	Registry* reg = sceneManager().getActiveRegistry();
	if (!reg || !reg->isAlive(m_flashlight))
		return;

	const CameraView& cam = cameraView();
	auto* tc = reg->getComponent<TransformComponent>(m_flashlight);
	auto* sl = reg->getComponent<SpotLightComponent>(m_flashlight);
	if (!tc || !sl)
		return;

	tc->setTranslation(cam.position);
	sl->setDirection(cam.forward);
}

// ─── UI Rendering ────────────────────────────────────────────────────────────

void Sandbox::renderGameModeOverlay() {
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	float padding = 10.0f;
	ImGui::SetNextWindowPos(
		ImVec2(vp->WorkPos.x + padding, vp->WorkPos.y + padding),
		ImGuiCond_Always, ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.5f);
	ImGui::SetNextWindowSize(ImVec2(0, 0));
	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoMove;

	if (ImGui::Begin("##ControlsOverlay", nullptr, flags)) {
		auto keyLabel = [](const char* key, const char* desc) {
			ImGui::Text("%s", key);
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::TextDisabled("  %s", desc);
		};

		ImGui::TextDisabled("Controls");
		ImGui::Separator();

		keyLabel("WASD", "Move");
		ImGui::SameLine(0.0f, 16.0f);
		keyLabel("Space", "Up");
		ImGui::SameLine(0.0f, 16.0f);
		keyLabel("C", "Down");
		ImGui::Separator();
		keyLabel("F", "Launch Firework");
		ImGui::SameLine(0.0f, 16.0f);
		keyLabel("K", "Customize Fireworks");
		ImGui::Separator();
		keyLabel("L", "Flashlight");
		ImGui::SameLine(0.0f, 16.0f);
		keyLabel("Tab", "Editor");
		ImGui::SameLine(0.0f, 16.0f);
		keyLabel("P", "Performance");
		ImGui::SameLine(0.0f, 16.0f);
		keyLabel("H", "Hide");
	}
	ImGui::End();
}

void Sandbox::renderUI() {
	if (m_show_fireworks_panel && !m_fireworks_panel_token)
		m_fireworks_panel_token = getInputController().acquireCursor();
	else if (!m_show_fireworks_panel && m_fireworks_panel_token)
		m_fireworks_panel_token.release();

	if (m_show_fireworks_panel)
		renderFireworksPanel();

	if (!getInputController().isEditorMode()) {
		if (m_show_controls)
			renderGameModeOverlay();
		return;
	}
}

void Sandbox::renderFireworksPanel() {
	if (!m_fireworks)
		return;

	ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Fireworks", &m_show_fireworks_panel))
		m_fireworks->drawConfigUI();
	ImGui::End();
}

} // namespace ve

// Called by the entry point to create the application instance
ve::VeApplication* createApp(std::filesystem::path project_root) {
	return new ve::Sandbox(project_root);
}