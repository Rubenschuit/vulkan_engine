#include "sandbox.hpp"
#include "application/ve_entry_point.hpp" // defines main() and createApp(), must be in exactly one TU

#include <imgui.h>

namespace ve {

static EngineConfig makeEngineConfig(const std::filesystem::path& root) {
	return EngineConfig{
		.app_name        = "Sandbox",
		.working_dir     = root,
		.shaders_dir     = root / "shaders",
		.skybox_dir      = root / "textures" / "skybox",
		.particle_assets = {
			.glow  = root / "textures" / "light.ktx2",
			.fire  = root / "textures" / "fire_ball.ktx",
			.smoke = root / "textures" / "smoke_atlas.ktx2",
		},
	};
}

Sandbox::Sandbox(const std::filesystem::path& working_dir)
	: VeApplication(makeEngineConfig(working_dir)),
	  m_paths(working_dir) {

	// Sync particle UI defaults from engine systems
	auto& ps = getParticleSystem();
	m_particles.mode            = ps.getMode();
	m_particles.speed           = ps.getSpeed();
	m_particles.pending_count   = ps.getPendingParticleCount();
	m_particles.velocity_mean   = ps.getMean();
	m_particles.velocity_stddev = ps.getStddev();
	m_particles.min_life        = ps.getMinLife();
	m_particles.max_life        = ps.getMaxLife();
	m_particles.should_respawn  = ps.getShouldRespawn();

	// Register scenes (Editor provides the UI to switch between these)
	registerScene("Simple", [this](const SceneContext& ctx) {
		return std::make_unique<SimpleScene>(ctx, m_paths);
	});
	registerAsyncScene("Sponza", m_paths.sponza_model(),
		[](const SceneContext& ctx, std::unique_ptr<VeModel> model) {
			return std::make_unique<SponzaScene>(ctx, std::move(model));
		});
	registerAsyncScene("Bistro", m_paths.bistro_model(),
		[](const SceneContext& ctx, std::unique_ptr<VeModel> model) {
			return std::make_unique<BistroScene>(ctx, std::move(model));
		}, true, true);

	// Load the first registered scene
	loadDefaultScene(0);

	registerInputActions();
}

Sandbox::~Sandbox() {
	eventBus().unsubscribe<InputActionEvent>(m_input_sub);
}

// ─── Input Actions ───────────────────────────────────────────────────────────

void Sandbox::registerInputActions() {
	auto& ic = getInputController();

	ic.registerAction({
		.name = "Reset Particles", .key = GLFW_KEY_E,
		.trigger = TriggerType::OnPress, .context = InputContext::GameMode,
		.description = "Reset particles at camera look position"
	});
	ic.registerAction({
		.name = "Reset Disc", .key = GLFW_KEY_G,
		.trigger = TriggerType::OnPress, .context = InputContext::GameMode,
		.description = "Reset particles as disc at camera look position"
	});
	ic.registerAction({
		.name = "Launch Firework", .key = GLFW_KEY_F,
		.trigger = TriggerType::OnPress, .context = InputContext::GameMode,
		.description = "Launch a firework rocket"
	});
	static const char* mode_names[] = {
		"Gravity Earth", "Cool", "Succ", "Stasis", "Galaxy Massive"
	};
	for (int i = 1; i <= 5; ++i) {
		ic.registerAction({
			.name = "Set Mode", .key = GLFW_KEY_0 + i,
			.trigger = TriggerType::OnPress, .context = InputContext::GameMode,
			.description = std::string(mode_names[i - 1]) + " (mode " + std::to_string(i) + ")",
			.value = static_cast<uint32_t>(i)
		});
	}

	ic.registerAction({
		.name = "Toggle Controls", .key = GLFW_KEY_H,
		.trigger = TriggerType::OnPress, .context = InputContext::Always,
		.description = "Toggle controls overlay"
	});

	m_input_sub = eventBus().subscribe<InputActionEvent>(
		[this](const InputActionEvent& e) {
			auto& ps = getParticleSystem();
			const auto& cv = m_current_camera_view;
			if (e.name == "Reset Particles") {
				ps.setOrigin(cv.forward * 100.0f + cv.position);
				ps.resetPoint();
			} else if (e.name == "Reset Disc") {
				ps.setOrigin(cv.forward * 100.0f + cv.position);
				ps.resetDisc();
			} else if (e.name == "Launch Firework") {
				getFireworksSystem().launchRocket();
			} else if (e.name == "Set Mode") {
				auto mode = static_cast<ParticleMode>(e.value);
				ps.setMode(mode);
				m_particles.mode = mode;
			} else if (e.name == "Toggle Controls") {
				m_show_controls = !m_show_controls;
			}
		});
}

// ─── Per-Frame Update ────────────────────────────────────────────────────────

void Sandbox::update() {
	updateParticles();
}

void Sandbox::updateParticles() {
	if (!isParticlesDeclared() && !isFireworksDeclared())
		return;

	auto& ps = getParticleSystem();
	auto& fw = getFireworksSystem();

	ps.setEnabled(isParticlesDeclared() && m_particles_enabled);
	fw.setEnabled(isFireworksDeclared() && m_fireworks_enabled);

	if (!isParticlesDeclared())
		return;

	// UI-driven config
	ps.stageParticleCount(m_particles.pending_count);
	if (m_particles.apply_count) {
		ps.applyStagedParticleCount();
		m_particles.apply_count = false;
	}
	if (m_particles.reset_count) {
		ps.scheduleRestart();
		m_particles.reset_count = false;
	}
	ps.setSpeed(m_particles.speed);
	ps.setMean(m_particles.velocity_mean);
	ps.setStddev(m_particles.velocity_stddev);
	ps.setLifeRange(m_particles.min_life, m_particles.max_life);
	ps.setShouldRespawn(m_particles.should_respawn);
}

// ─── UI Rendering ────────────────────────────────────────────────────────────

void Sandbox::renderGameModeOverlay() {
	static const char* mode_labels[] = {
		"Gravity Earth", "Cool", "Succ", "Stasis", "Galaxy Massive"
	};

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
		keyLabel("E", "Reset Particles");
		ImGui::SameLine(0.0f, 16.0f);
		keyLabel("G", "Reset Disc");
		keyLabel("F", "Launch Firework");
		int mode_idx = static_cast<int>(m_particles.mode) - 1;
		const char* label = (mode_idx >= 0 && mode_idx < 5) ? mode_labels[mode_idx] : "Unknown";
		ImGui::Text("1-5");
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::TextDisabled("  Mode %d: %s", mode_idx + 1, label);
		ImGui::Separator();
		keyLabel("Tab", "Editor");
		ImGui::SameLine(0.0f, 16.0f);
		keyLabel("P", "Performance");
		ImGui::SameLine(0.0f, 16.0f);
		keyLabel("H", "Hide");
	}
	ImGui::End();
}

void Sandbox::renderUI() {
	if (!getInputController().isEditorMode()) {
		if (m_show_controls)
			renderGameModeOverlay();
		return;
	}

	auto& app_visible = getEditor().getState().show_app_settings;
	if (ImGui::Begin(getAppSettingsWindowName().c_str(), &app_visible, ImGuiWindowFlags_NoFocusOnAppearing)) {
		if (!isParticlesDeclared() && !isFireworksDeclared()) {
			ImGui::TextDisabled("The active scene does not declare any content subsystems.");
		} else if (ImGui::BeginTabBar("SettingsTabs")) {
			if (isParticlesDeclared() && ImGui::BeginTabItem("Particle")) {
				ImGui::Checkbox("Enabled##particles", &m_particles_enabled);
				ImGui::Separator();

				int count = static_cast<int>(m_particles.pending_count);

				ImGui::Text("Simulation Control");
				ImGui::Separator();
				ImGui::SliderInt("Active Count", &count, 1000, 5000000);
				if (count < 1) count = 1;
				m_particles.pending_count = static_cast<uint32_t>(count);
				if (ImGui::Button("Apply Count"))
					m_particles.apply_count = true;
				ImGui::SameLine();
				if (ImGui::Button("Reset System"))
					m_particles.reset_count = true;

				ImGui::SliderFloat("Speed", &m_particles.speed, 0, 10);
				ImGui::SameLine();
				if (ImGui::Button("1.0x")) m_particles.speed = 1.0f;

				ImGui::Separator();
				ImGui::Text("Lifetime");
				ImGui::Separator();
				ImGui::SliderFloat("Min Life", &m_particles.min_life, 0.1f, 100.0f);
				ImGui::SliderFloat("Max Life", &m_particles.max_life, 0.1f, 100.0f);
				ImGui::Checkbox("Respawn", &m_particles.should_respawn);
				if (m_particles.min_life > m_particles.max_life)
					m_particles.min_life = m_particles.max_life;

				ImGui::Separator();
				ImGui::Text("Physics / Explosion");
				ImGui::Separator();
				ImGui::SliderFloat("Mean Velocity", &m_particles.velocity_mean, -60, 60);
				ImGui::SliderFloat("StdDev Velocity", &m_particles.velocity_stddev, 0, 60);

				ImGui::EndTabItem();
			}
			if (isFireworksDeclared() && ImGui::BeginTabItem("Fireworks")) {
				ImGui::Checkbox("Enabled##fireworks", &m_fireworks_enabled);
				ImGui::Separator();

				auto& config = getFireworksSystem().getConfig();

				ImGui::Text("Launch Settings");
				ImGui::Separator();
				ImGui::DragFloat3("Launch Position", &config.launch_pos.x);
				ImGui::DragFloatRange2("Launch Velocity", &config.launch_vel_min, &config.launch_vel_max, 1.0f, 0.0f, 500.0f);
				ImGui::SliderFloat("Spread XY", &config.launch_spread_xy, 0.0f, 100.0f);
				ImGui::SliderInt("Launch Count", &config.launch_count, 1, 100);

				ImGui::Text("Colors");
				ImGui::Checkbox("Random Color", &config.use_random_color);
				if (!config.use_random_color)
					ImGui::ColorEdit4("Particle Color", &config.particle_color.r);

				ImGui::Separator();
				ImGui::Text("Explosion");
				ImGui::SliderInt("Particle Count", &config.explosion_particle_count, 5, 3000);
				ImGui::SliderFloat("Particle Size", &config.explosion_size, 0.1f, 5.0f);
				ImGui::SliderFloat("Trail Interval", &config.trail_interval, 0.0001f, 0.1f, "%.4f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Interval at which to emit smoke/trail particles.");

				ImGui::Separator();
				ImGui::Text("Environment");
				ImGui::DragFloat3("Wind Direction", &config.wind_direction.x, 0.05f, -1.0f, 1.0f);
				ImGui::SliderFloat("Wind Strength", &config.wind_strength, 0.0f, 50.0f);
				ImGui::SliderFloat("Gravity", &config.gravity, 0.0f, 50.0f);

				ImGui::Separator();
				ImGui::Text("System");
				ImGui::SliderInt("Particle Capacity", &config.max_particles, 1000, 5000000);
				if (ImGui::Button("Apply Capacity"))
					getFireworksSystem().setParticleCapacity(static_cast<uint32_t>(config.max_particles));

				if (ImGui::Button("Launch Rocket", ImVec2(-1, 0)))
					getFireworksSystem().launchRocket();

				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

} // namespace ve

// Called by the entry point to create the application instance
ve::VeApplication* createApp(std::filesystem::path project_root) {
	return new ve::Sandbox(project_root);
}
