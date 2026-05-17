#pragma once

#include "application/ve_engine_config.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_device.hpp"
#include "rendering/ve_renderer.hpp"
#include "ui/imgui_layer.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "input/input_controller.hpp"
#include "scene/camera_view.hpp"
#include "scene/scene_manager.hpp"
#include "resources/ve_resource_manager.hpp"
#include "physics/physics_system.hpp"
#include "events/event_bus.hpp"
#include <memory>
#include <chrono>
#include <functional>
#include <filesystem>

namespace ve {

class ParticleSystem;
class FireworksSystem;
class SkyboxRenderSystem;
class PbrRenderSystem;
class Editor;
class VeScene;
class VeModel;
class RenderPipeline;
class RenderResources;
struct SceneContext;

class VENGINE_API VeApplication {
public:

	static constexpr int WIDTH = 1920;
	static constexpr int HEIGHT = 1080;
	const char* APP_NAME = "Vulkan Engine!";

	explicit VeApplication(const EngineConfig& config);
	virtual ~VeApplication();

	VeApplication(const VeApplication&) = delete;
	VeApplication& operator=(const VeApplication&) = delete;

	// Called by main in entry point to start the application loop
	void run();

protected:
	// --- App overrides ---
	virtual void update() {}
	virtual void renderUI() {}

	// --- Scene management ---
	void registerScene(std::string name,
	                   std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory);
	void registerGltfScene(std::string name,
	                       std::filesystem::path gltf_path,
	                       std::function<std::unique_ptr<VeScene>(const SceneContext&, std::unique_ptr<VeModel>)> factory,
	                       bool extract_lights = true,
	                       bool flip_tex_coord_v = false);
	void loadDefaultScene(int index);
	SceneManager& sceneManager() { return *m_scene_manager; }

	// --- Engine accessors ---
	EventBus& eventBus() { return m_event_bus; }
	UIContext& ui() { return m_ui; }
	InputController& getInputController() { return m_input_controller; }
	Editor& getEditor() { return *m_editor; }
	PhysicsSystem& getPhysicsSystem() { return *m_physics_system; }
	ParticleSystem& getParticleSystem();
	FireworksSystem& getFireworksSystem();
	SkyboxRenderSystem& getSkyboxSystem();
	PbrRenderSystem& getPbrSystem();
	bool isParticlesDeclared() const;
	bool isFireworksDeclared() const;
	const std::string& getAppSettingsWindowName() const;
	const CameraView& cameraView() const { return m_current_camera_view; }

private:
	// --- Engine state ---
	VeWindow m_ve_window;
	VeDevice m_ve_device;
	EventBus m_event_bus;
	VeResourceManager m_resource_manager;
	VeRenderer m_ve_renderer;
	CameraView m_current_camera_view;
	InputController m_input_controller;

	// --- Initialisation ---
	void initSystems();
	void initEditor();

	// --- Per-frame helpers ---
	void updateCamera(float fov_radians);
	void updateFrameTime();
	void setWindowTitle();

	EngineConfig m_config;

	// --- UI / engine state ---
	RenderSettings m_settings;
	FrameStats m_stats;
	SimulationSettings m_sim;
	UIContext m_ui{m_settings, m_stats, m_sim};
	std::unique_ptr<ImGuiLayer> m_imgui_layer;
	std::unique_ptr<Editor> m_editor;

	EventSubscriptionId m_input_action_sub = 0;
	EventSubscriptionId m_scene_loaded_sub = 0;

	// --- Systems ---
	std::unique_ptr<RenderResources> m_render_resources;
	std::unique_ptr<RenderPipeline> m_render_pipeline;
	std::unique_ptr<PhysicsSystem> m_physics_system;
	std::unique_ptr<SceneManager> m_scene_manager;

	// --- Camera ---
	float m_fov = glm::radians(80.0f);
	float m_near_plane = 0.1f;
	float m_far_plane = 100000.0f;
	float m_last_aspect{0.0f};

	// --- Timing ---
	using clock = std::chrono::steady_clock;
	clock::time_point m_last_frame_time{clock::now()};
	float m_total_time{0.0f};
	float m_frame_time{0.0f};
};

}
