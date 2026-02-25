#include "sandbox.hpp"
#include "application/ve_entry_point.hpp" // defines main() and createApp(), must be in exactly one TU

#include <imgui.h>

namespace ve {

static EngineConfig makeEngineConfig(const std::filesystem::path& root) {
	return EngineConfig{
		.app_name          = "Sandbox",
		.working_dir       = root,
		.shaders_dir       = root / "shaders",
		.skybox_dir        = root / "textures" / "skybox",
		.cube_model        = root / "models" / "cube.gltf",
		.particle_texture  = root / "textures" / "light.ktx2",
		.fire_texture      = root / "textures" / "fire_ball.ktx",
		.smoke_texture     = root / "textures" / "smoke_atlas.ktx2",
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
	registerScene("Sponza", [this](const SceneContext& ctx) {
		return std::make_unique<SponzaScene>(ctx, m_paths);
	});
	registerScene("Bistro", [this](const SceneContext& ctx) {
		return std::make_unique<BistroScene>(ctx, m_paths);
	});

	// Load the first registered scene
	loadDefaultScene(0);
}

// ─── Per-Frame Update ────────────────────────────────────────────────────────

void Sandbox::update() {
	updateParticles(getInputActions());
}

void Sandbox::updateParticles(const InputActions& actions) {
	auto& ps = getParticleSystem();
	auto& fw = getFireworksSystem();

	ps.setEnabled(m_particles_enabled);
	fw.setEnabled(m_fireworks_enabled);

	// Keyboard actions
	if (actions.set_mode >= 1 && actions.set_mode <= 5) {
		ParticleMode mode = static_cast<ParticleMode>(actions.set_mode);
		ps.setMode(mode);
		m_particles.mode = mode;
	}
	if (actions.reset_particles) {
		ps.setOrigin(m_camera.getForward() * 100.0f + m_camera.getPosition());
		ps.resetPoint();
	} else if (actions.reset_disc) {
		ps.setOrigin(m_camera.getForward() * 100.0f + m_camera.getPosition());
		ps.resetDisc();
	}
	if (actions.launch_firework)
		fw.launchRocket();

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

void Sandbox::renderUI() {
	if (ImGui::Begin(getAppSettingsWindowName().c_str(), nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
		if (ImGui::BeginTabBar("SettingsTabs")) {
			if (ImGui::BeginTabItem("Particle")) {
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
			if (ImGui::BeginTabItem("Fireworks")) {
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
