/* SceneManager owns the scene registry, the active scene, and the async asset
 * loader. It drives
 *
 *   - Scene swap   (SceneLoadRequestedEvent): switch to a registered scene
 *                  or create a fresh empty scene; emits SceneLoadedEvent /
 *                  SceneUnloadedEvent on transitions.
 *   - Model import (AddModelRequestedEvent):  async load a gltf and instantiate
 *                  it into the active scene's registry at the requested
 *                  placement; emits AssetLoadCompleteEvent when done
 *
 * VeApplication constructs this after RenderResources and RenderPipeline and
 * ticks it once per frame.
 */
#pragma once
#include "ve_export.hpp"
#include "events/event_bus.hpp"
#include "scene/ve_entity.hpp"

#include <deque>
#include <filesystem>
#include <functional>
#include <glm/vec3.hpp>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class AssetLoadingSystem;
class Registry;
class RenderResources;
class VeModel;
class VeResourceManager;
class VeScene;
struct SceneContext;

struct VENGINE_API SceneEntry {
	std::string name;
	std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory;
};

// scene_index >= 0 activates the registered scene at that index.
// scene_index <  0 creates a fresh empty scene.
struct SceneLoadRequestedEvent {
	int scene_index = -1;
};

// Async load and add a gltf to the current scene at the given placement.
// Optional on_loaded(Entity wrapper) runs after instantiation into the registry.
struct AddModelRequestedEvent {
	std::filesystem::path gltf_path;
	glm::vec3 translation = {0.f, 0.f, 0.f};
	glm::vec3 rotation = {0.f, 0.f, 0.f};
	glm::vec3 scale = {1.f, 1.f, 1.f};
	bool extract_lights = true;
	bool flip_tex_coord_v = false;
	std::function<void(Entity)> on_loaded = nullptr;
};

class VENGINE_API SceneManager {
public:
	SceneManager(VeResourceManager& resource_manager,
	             RenderResources& render_resources,
	             EventBus& event_bus);
	~SceneManager();

	SceneManager(const SceneManager&) = delete;
	SceneManager& operator=(const SceneManager&) = delete;

	void registerScene(std::string name,
	                   std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory);

	void loadDefaultScene(int index);
	void setActiveScene(std::unique_ptr<VeScene> scene);
	void unloadScene();

	// Per-frame tick: processes pending deletions, drives the scene-swap /
	// async-loader state machine, and ticks the active scene's update.
	void tick(float frame_time);

	VeScene* getActiveScene() { return m_active_scene.get(); }
	Registry* getActiveRegistry();
	// True when a scene is active and no scene swap or model load is pending
	// or in flight.
	bool isIdle() const {
		return m_active_scene != nullptr && m_loading_scene == nullptr
			&& m_pending_op == PendingOp::NONE && m_async_op == PendingOp::NONE
			&& m_pending_add_models.empty();
	}
	SceneContext makeContext();
	const std::vector<SceneEntry>& entries() const { return m_scene_entries; }
	int loadedSceneIndex() const { return m_loaded_scene_index; }
	int loadingSceneIndex() const { return m_loading_scene_index; }
	AssetLoadingSystem& assetLoader() { return *m_asset_loader; }

private:
	enum class PendingOp { NONE, LOAD_REGISTERED, NEW_EMPTY, ADD_MODEL };

	void processPending();
	void tickAsyncLoader();
	void finalizeAsyncLoad();
	void instantiateAndEmit(const VeModel& model, VeScene& target,
	                        const AddModelRequestedEvent& req);

	VeResourceManager& m_resource_manager;
	RenderResources& m_render_resources;
	EventBus& m_event_bus;

	std::vector<SceneEntry> m_scene_entries;
	int m_loaded_scene_index = -1;
	std::unique_ptr<VeScene> m_active_scene;

	std::unique_ptr<VeScene> m_loading_scene;
	int m_loading_scene_index = -1;
	bool m_constructing_scene = false;
	bool m_loading_scene_awaits_content = false;

	// Scene-swap ops are single-slot
	PendingOp m_pending_op = PendingOp::NONE;
	int m_pending_scene_index = -1;

	// Model adds are queued; one is processed per tick when the loader is idle.
	// A scene swap discards the queue.
	std::deque<AddModelRequestedEvent> m_pending_add_models;

	std::unique_ptr<AssetLoadingSystem> m_asset_loader;
	PendingOp m_async_op = PendingOp::NONE;
	AddModelRequestedEvent m_async_add_model;

	EventSubscriptionId m_scene_load_sub = 0;
	EventSubscriptionId m_add_model_sub = 0;
};

}