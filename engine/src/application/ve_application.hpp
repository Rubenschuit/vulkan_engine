#pragma once

#include "application/ve_engine_config.hpp"
#include "application/benchmark_runner.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_device.hpp"
#include "rendering/ve_renderer.hpp"
#include "ui/ui_context.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "input/input_controller.hpp"
#include "input/character_input_driver.hpp"
#include "scene/camera_view.hpp"
#include "scene/camera_manager.hpp"
#include "scene/scene_manager.hpp"
#include "resources/ve_resource_manager.hpp"
#include "physics/physics_system.hpp"
#include "events/event_bus.hpp"
#include <memory>
#include <chrono>
#include <functional>
#include <filesystem>

namespace ve {

class Editor;
class VeScene;
class RenderPipeline;
class RenderResources;
struct SceneContext;
struct RenderServices;

class VENGINE_API VeApplication {
public:

	explicit VeApplication(const EngineConfig& config);
	virtual ~VeApplication();

	VeApplication(const VeApplication&) = delete;
	VeApplication& operator=(const VeApplication&) = delete;

	// Called by main in entry point to start the application loop
	void run();

	// Benchmark mode is armed by main() before createApp(); the constructor
	// consumes the pending config. See BenchmarkConfig::parseArgs for flags.
	static void setPendingBenchmark(BenchmarkConfig config);
	int exitCode() const { return m_exit_code; }

protected:
	// --- App overrides ---
	virtual void update() {}
	virtual void renderUI() {}

	// --- Scene management ---
	void registerScene(std::string name,
	                   std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory);
	void loadDefaultScene(int index);
	SceneManager& sceneManager() { return *m_scene_manager; }

	// --- Engine accessors ---
	EventBus& eventBus() { return m_event_bus; }
	InputController& getInputController() { return m_input_controller; }
	VeWindow& getWindow() { return m_ve_window; }
	Editor& getEditor() { return *m_editor; }
	PhysicsSystem& getPhysicsSystem() { return *m_physics_system; }
	const RenderServices& renderServices() const;
	const CameraView& cameraView() const;
	float frameTime() const { return m_frame_time; }
	float totalTime() const { return m_total_time; }
	VeResourceManager& resourceManager() { return m_resource_manager; }

private:
	VeWindow m_ve_window;
	VeDevice m_ve_device;
	EventBus m_event_bus;
	VeResourceManager m_resource_manager;
	VeRenderer m_ve_renderer;
	InputController m_input_controller;

	void initSystems();
	void initEditor();
	void setupBenchmark();
	void finishBenchmark();

	void updateFrameTime();
	void setWindowTitle();

	const EngineConfig m_config;

	// --- App-owned state ---
	SimulationSettings m_sim;
	CameraManager m_cameras;
	CharacterInputDriver m_character_input;
	std::unique_ptr<Editor> m_editor;
	std::unique_ptr<BenchmarkRunner> m_benchmark;
	bool m_benchmark_scene_started = false;
	bool m_benchmark_config_error = false;
	int m_exit_code = 0;
	EventSubscriptionId m_input_action_sub = 0;
	EventSubscriptionId m_scene_loaded_sub = 0;

	// --- Systems ---
	std::unique_ptr<RenderResources> m_render_resources;
	std::unique_ptr<RenderPipeline> m_render_pipeline;
	std::unique_ptr<PhysicsSystem> m_physics_system;
	std::unique_ptr<SceneManager> m_scene_manager;

	// --- Timing ---
	using clock = std::chrono::steady_clock;
	clock::time_point m_last_frame_time{clock::now()};
	float m_total_time{0.0f};
	float m_frame_time{0.0f};
};

}
