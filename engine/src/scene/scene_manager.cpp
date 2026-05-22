#include "pch.hpp"
#include "scene/scene_manager.hpp"

#include "events/engine_events.hpp"
#include "rendering/render_resources.hpp"
#include "resources/asset_loading_system.hpp"
#include "resources/ve_model.hpp"
#include "resources/scene_instantiation.hpp"
#include "resources/ve_resource_manager.hpp"
#include "scene/gltf_scene.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_scene.hpp"
#include "utils/ve_log.hpp"

namespace ve {

SceneManager::SceneManager(VeResourceManager& resource_manager,
                           RenderResources& render_resources,
                           EventBus& event_bus)
	: m_resource_manager(resource_manager),
	  m_render_resources(render_resources),
	  m_event_bus(event_bus) {
	m_asset_loader = std::make_unique<AssetLoadingSystem>(m_resource_manager);

	m_scene_load_sub = m_event_bus.subscribe<SceneLoadRequestedEvent>(
		[this](const SceneLoadRequestedEvent& e) {
			if (m_pending_op != PendingOp::NONE)
				VE_LOGW("SceneManager: SceneLoadRequestedEvent overwrites pending op");
			if (e.scene_index >= 0) {
				m_pending_op = PendingOp::LOAD_REGISTERED;
				m_pending_scene_index = e.scene_index;
			} else {
				m_pending_op = PendingOp::NEW_EMPTY;
				m_pending_scene_index = -1;
			}
		});

	m_add_model_sub = m_event_bus.subscribe<AddModelRequestedEvent>(
		[this](const AddModelRequestedEvent& e) {
			m_pending_add_models.push_back(e);
			if (m_constructing_scene)
				m_loading_scene_awaits_content = true;
		});
}

SceneManager::~SceneManager() {
	m_event_bus.unsubscribe<SceneLoadRequestedEvent>(m_scene_load_sub);
	m_event_bus.unsubscribe<AddModelRequestedEvent>(m_add_model_sub);
	unloadScene();
}

SceneContext SceneManager::makeContext() {
	return m_render_resources.makeSceneContext();
}

Registry* SceneManager::getActiveRegistry() {
	return m_active_scene ? &m_active_scene->getRegistry() : nullptr;
}

void SceneManager::registerScene(std::string name,
                                 std::function<std::unique_ptr<VeScene>(const SceneContext&)> factory) {
	m_scene_entries.push_back({std::move(name), std::move(factory)});
}

void SceneManager::loadDefaultScene(int index) {
	if (index < 0 || index >= static_cast<int>(m_scene_entries.size()))
		return;
	const auto& entry = m_scene_entries[static_cast<size_t>(index)];
	if (!entry.factory) {
		VE_LOGW("loadDefaultScene: entry " + std::to_string(index) + " has no factory");
		return;
	}
	auto ctx = makeContext();
	m_loaded_scene_index = index;
	setActiveScene(entry.factory(ctx));
}

void SceneManager::setActiveScene(std::unique_ptr<VeScene> scene) {
	unloadScene();
	m_active_scene = std::move(scene);
	if (m_active_scene) {
		m_event_bus.emitImmediate(SceneLoadedEvent{&m_active_scene->getRegistry(), m_active_scene.get()});
		VE_LOGI("SceneManager: loaded scene '" << m_active_scene->getName() << "' with "
			<< m_active_scene->getRegistry().entityCount() << " entities");
	}
}

void SceneManager::unloadScene() {
	if (!m_active_scene)
		return;
	// SceneResourceManager subscribes to SceneUnloadedEvent and does a
	// vkDeviceWaitIdle before clearing its caches, so the GPU is idle here.
	VE_LOGI("SceneManager: unloading scene '" << m_active_scene->getName() << "'");
	m_event_bus.emitImmediate(SceneUnloadedEvent{});
	m_active_scene.reset();
	m_resource_manager.flushPendingUnloads();
}

void SceneManager::tick(float frame_time) {
	// Resource handles dropped here get queued on VeResourceManager's rescue
	// queue; their underlying GPU resources are destroyed
	// MAX_FRAMES_IN_FLIGHT frames later on tickFrame.
	if (m_active_scene) {
		auto& registry = m_active_scene->getRegistry();
		if (registry.hasPendingDeletions())
			registry.processPendingDeletions();
	}

	processPending();
	tickAsyncLoader();

	if (m_active_scene)
		m_active_scene->update(frame_time);
}

void SceneManager::processPending() {
	if (m_pending_op != PendingOp::NONE) {
		PendingOp op = m_pending_op;
		m_pending_op = PendingOp::NONE;

		auto ctx = makeContext();
		switch (op) {
			case PendingOp::LOAD_REGISTERED: {
				int idx = m_pending_scene_index;
				int intended = m_loading_scene_index >= 0 ? m_loading_scene_index : m_loaded_scene_index;
				if (idx >= 0 && idx < static_cast<int>(m_scene_entries.size()) && idx != intended) {
					const auto& entry = m_scene_entries[static_cast<size_t>(idx)];
					if (entry.factory) {
						m_asset_loader->cancel();
						m_pending_add_models.clear();
						m_async_op = PendingOp::NONE;
						m_loading_scene.reset();
						m_loading_scene_index = -1;
						m_loading_scene_awaits_content = false;
						m_constructing_scene = true;
						auto new_scene = entry.factory(ctx);
						m_constructing_scene = false;
						if (m_loading_scene_awaits_content) {
							m_loading_scene = std::move(new_scene);
							m_loading_scene_index = idx;
						} else {
							setActiveScene(std::move(new_scene));
							m_loaded_scene_index = idx;
						}
					}
				}
				break;
			}
			case PendingOp::NEW_EMPTY: {
				m_asset_loader->cancel();
				m_pending_add_models.clear();
				m_async_op = PendingOp::NONE;
				m_loading_scene.reset();
				m_loading_scene_index = -1;
				m_loaded_scene_index = -1;
				setActiveScene(std::make_unique<GltfScene>(ctx));
				break;
			}
			case PendingOp::ADD_MODEL:
			case PendingOp::NONE:
				break;
		}
		return;
	}

	// Drain one queued model add per tick, but only while the loader is free.
	if (m_async_op != PendingOp::NONE)
		return;
	if (m_pending_add_models.empty())
		return;

	AddModelRequestedEvent req = std::move(m_pending_add_models.front());
	m_pending_add_models.pop_front();

	VeScene* target = m_loading_scene ? m_loading_scene.get() : m_active_scene.get();
	if (!target)
		return;

	// Cache hit: instantiate immediately
	std::string cache_key = req.gltf_path.lexically_normal().generic_string();
	if (auto cached = m_resource_manager.tryGetHandle<VeModel>(cache_key); cached.isValid()) {
		instantiateAndEmit(*cached, *target, req);
		if (m_loading_scene) {
			setActiveScene(std::move(m_loading_scene));
			m_loaded_scene_index = m_loading_scene_index;
			m_loading_scene_index = -1;
		}
		return;
	}

	m_asset_loader->beginModelLoad(req.gltf_path, req.extract_lights, req.flip_tex_coord_v);
	m_async_op = PendingOp::ADD_MODEL;
	m_async_add_model = std::move(req);
}

void SceneManager::tickAsyncLoader() {
	LoadState state = m_asset_loader->getState();


	bool external_cancel = state == LoadState::IDLE && m_async_op != PendingOp::NONE;
	bool load_failed = state == LoadState::FAILED;
	if (external_cancel || load_failed) {
		if (load_failed) {
			VE_LOGE("SceneManager: async load failed; discarding staged scene");
			m_asset_loader->cancel();
		} else {
			VE_LOGI("SceneManager: async load cancelled; discarding staged scene");
		}
		m_pending_add_models.clear();
		m_async_op = PendingOp::NONE;
		m_async_add_model = {};
		m_loading_scene.reset();
		m_loading_scene_index = -1;
		return;
	}

	if (state == LoadState::IDLE)
		return;

	m_asset_loader->tick();

	if (m_asset_loader->getState() == LoadState::READY)
		finalizeAsyncLoad();
}

void SceneManager::finalizeAsyncLoad() {
	auto model = m_asset_loader->takeModel();
	VeScene* target = m_loading_scene ? m_loading_scene.get() : m_active_scene.get();
	if (!model || !target) {
		m_async_op = PendingOp::NONE;
		return;
	}

	// Register so concurrent / back-to-back placeModel calls hit the cache.
	// The handle is dropped at end of scope; if nothing else holds it the
	// VeModel retires after MAX_FRAMES_IN_FLIGHT ticks.
	const std::string& key = m_asset_loader->getCacheKey();
	auto handle = m_resource_manager.registerExisting<VeModel>(key, std::move(model));

	instantiateAndEmit(*handle, *target, m_async_add_model);
	m_async_op = PendingOp::NONE;
	m_async_add_model = {};

	if (m_loading_scene) {
		setActiveScene(std::move(m_loading_scene));
		m_loaded_scene_index = m_loading_scene_index;
		m_loading_scene_index = -1;
	}
}

void SceneManager::instantiateAndEmit(const VeModel& model, VeScene& target,
                                      const AddModelRequestedEvent& req) {
	std::string wrapper_name = req.gltf_path.stem().string();
	Entity wrapper = ve::instantiateModel(model, target.getRegistry(),
		wrapper_name, req.translation, req.rotation, req.scale);

	if (req.on_loaded)
		req.on_loaded(wrapper);

	m_event_bus.emitImmediate(AssetLoadCompleteEvent{
		req.gltf_path.filename().string(), req.gltf_path, wrapper});
}

} // namespace ve