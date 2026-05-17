/*
 * Sandbox is an example application demonstrating some of the engine's
 * capabilities.
 *
 * It includes three scenes: a simple one with basic primitives,
 * the Sponza atrium, and a Bistro restaurant scene. The user can switch
 * between them at runtime.
 * There is also a fireworks effect that can be launched with a key press.
 *
 */
#pragma once
#include "VEngine/VEngine.hpp"
#include "asset_paths.hpp"
#include "scenes/bistro_scene.hpp"
#include "scenes/simple_scene.hpp"
#include "scenes/sponza_scene.hpp"
#include "effects/fireworks.hpp"
#include <filesystem>
#include <memory>

namespace ve {

class Sandbox : public VeApplication {
public:
	explicit Sandbox(const std::filesystem::path& working_dir);
	~Sandbox() override;

protected:
	void update() override;
	void renderUI() override;

private:
	AssetPaths m_paths;

	void registerInputActions();
	void renderGameModeOverlay();
	void renderFireworksPanel();

	bool m_show_controls = true;
	bool m_show_fireworks_panel = false;
	InputController::CursorCaptureToken m_fireworks_panel_token;
	EventSubscriptionId m_input_sub = 0;
	std::unique_ptr<effects::Fireworks> m_fireworks;
};

}

// Called by the entry point to create the application instance
ve::VeApplication* createApp(std::filesystem::path project_root);
