/* SceneManager owns the scene registry, the active scene, and the async asset
 * loader. It drives 
 *
 *   - Scene swap   (SceneLoadRequestedEvent): switch to a registered scene or
 *                  create a new empty scene; emits SceneLoadedEvent /
 *                  SceneUnloadedEvent on transitions.
 *   - Model import (AddModelRequestedEvent):  async load a gltf and add it
 *                  to the currently active scene's ECS; emits
 *                  AssetLoadCompleteEvent when done.
 *
 * Only one operation is in flight at a time.
 *
 * VeApplication constructs this after RenderResources and RenderPipeline and
 * ticks it once per frame.
 */
#pragma once
#include "ve_export.hpp"
#include "events/event_bus.hpp"

#include <filesystem>
#include <functional>
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
	std::filesystem::path gltf_path;
	std::function<std::unique_ptr<VeScene>(const SceneContext&, std::unique_ptr<VeModel>)> gltf_factory;
	bool extract_lights = true;
	bool flip_tex_coord_v = false;
};

// scene_index >= 0 activates the registered scene at that index.
// scene_index <  0 creates a fresh empty scene.
struct SceneLoadRequestedEvent {
	int scene_index = -1;
};

// Async load and add a gltf to the current scene
struct AddModelRequestedEvent {
	std::filesystem::path gltf_path;
	bool flip_tex_coord_v = false;
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

	// Register a scene from a gltf file
	void registerGltfScene(std::string name,
	                       std::filesystem::path gltf_path,
	                       std::function<std::unique_ptr<VeScene>(const SceneContext&, std::unique_ptr<VeModel>)> factory,
	                       bool extract_lights = true,
	                       bool flip_tex_coord_v = false);

	void loadDefaultScene(int index);
	void setActiveScene(std::unique_ptr<VeScene> scene);
	void unloadScene();

	// Per-frame tick: processes pending deletions, drives the scene-swap /
	// async-loader state machine, and ticks the active scene's update.
	void tick(float frame_time);

	VeScene* getActiveScene() { return m_active_scene.get(); }
	Registry* getActiveRegistry();
	SceneContext makeContext();
	const std::vector<SceneEntry>& entries() const { return m_scene_entries; }
	int loadedSceneIndex() const { return m_loaded_scene_index; }
	AssetLoadingSystem& assetLoader() { return *m_asset_loader; }

private:
	enum class PendingOp { NONE, LOAD_REGISTERED, NEW_EMPTY, ADD_MODEL };

	void processPending();
	void tickAsyncLoader();
	void finalizeAsyncLoad();

	VeResourceManager& m_resource_manager;
	RenderResources& m_render_resources;
	EventBus& m_event_bus;

	std::vector<SceneEntry> m_scene_entries;
	int m_loaded_scene_index = -1;
	std::unique_ptr<VeScene> m_active_scene;

	PendingOp m_pending_op = PendingOp::NONE;
	int m_pending_scene_index = -1;
	std::filesystem::path m_pending_gltf_path;
	bool m_pending_flip_v = false;

	std::unique_ptr<AssetLoadingSystem> m_asset_loader;
	PendingOp m_async_op = PendingOp::NONE;
	int m_async_scene_index = -1;

	EventSubscriptionId m_scene_load_sub = 0;
	EventSubscriptionId m_add_model_sub = 0;
};

}