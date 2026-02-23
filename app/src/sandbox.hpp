#pragma once
#include "VEngine/VEngine.hpp"
#include "asset_paths.hpp"
#include "scenes/bistro_scene.hpp"
#include "scenes/simple_scene.hpp"
#include "scenes/sponza_scene.hpp"
#include <filesystem>

namespace ve {

class Sandbox : public VeApplication {
public:
	explicit Sandbox(const std::filesystem::path& working_dir);
	~Sandbox() override = default;

protected:
	void update() override;
	void renderUI() override;

private:
	AssetPaths m_paths;

	// Particle/fireworks toggles
	bool m_particles_enabled = true;
	bool m_fireworks_enabled = true;

	// Particle config (app-specific UI state)
	struct ParticleConfig {
		ParticleMode mode = ParticleMode::COOL;
		float speed = 1.0f;
		uint32_t pending_count = 10000;
		bool apply_count = false;
		bool reset_count = false;
		float velocity_mean = 0.0f;
		float velocity_stddev = 1.0f;
		float min_life = 1.0f;
		float max_life = 3.0f;
		bool should_respawn = true;
	};
	ParticleConfig m_particles;

	void updateParticles(const InputActions& actions);
};

} // namespace ve

// Called by the entry point to create the application instance
ve::VeApplication* createApp(std::filesystem::path project_root);
