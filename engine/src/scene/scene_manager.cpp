#include "pch.hpp"
#include "scene/scene_manager.hpp"

#include "events/engine_events.hpp"
#include "rendering/render_resources.hpp"
#include "resources/asset_loading_system.hpp"
#include "resources/ve_model.hpp"
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
			if (!m_active_scene)
				return;
			m_pending_op = PendingOp::ADD_MODEL;
			m_pending_gltf_path = e.gltf_path;
			m_pending_flip_v = e.flip_tex_coord_v;
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
	m_scene_entries.push_back({std::move(name), std::move(factory), {}, {}, true, false});
}

void SceneManager::registerGltfScene(std::string name,
                                     std::filesystem::path gltf_path,
                                     std::function<std::unique_ptr<VeScene>(const SceneContext&, std::unique_ptr<VeModel>)> factory,
                                     bool extract_lights, bool flip_tex_coord_v) {
	m_scene_entries.push_back({std::move(name), {}, std::move(gltf_path), std::move(factory),
	                           extract_lights, flip_tex_coord_v});
}

void SceneManager::loadDefaultScene(int index) {
	if (index < 0 || index >= static_cast<int>(m_scene_entries.size()))
		return;
	const auto& entry = m_scene_entries[static_cast<size_t>(index)];
	if (!entry.factory) {
		VE_LOGW("loadDefaultScene: entry " + std::to_string(index) + " has no sync factory");
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
	if (m_pending_op == PendingOp::NONE)
		return;

	auto ctx = makeContext();
	switch (m_pending_op) {
		case PendingOp::LOAD_REGISTERED: {
			int idx = m_pending_scene_index;
			if (idx >= 0 && idx < static_cast<int>(m_scene_entries.size()) && idx != m_loaded_scene_index) {
				const auto& entry = m_scene_entries[static_cast<size_t>(idx)];
				if (!entry.gltf_path.empty() && entry.gltf_factory) {
					m_asset_loader->beginModelLoad(entry.gltf_path, entry.extract_lights, entry.flip_tex_coord_v);
					m_async_op = PendingOp::LOAD_REGISTERED;
					m_async_scene_index = idx;
				} else if (entry.factory) {
					m_asset_loader->cancel();
					m_loaded_scene_index = idx;
					setActiveScene(entry.factory(ctx));
				}
			}
			break;
		}
		case PendingOp::NEW_EMPTY: {
			m_asset_loader->cancel();
			m_loaded_scene_index = -1;
			setActiveScene(std::make_unique<GltfScene>(ctx));
			break;
		}
		case PendingOp::ADD_MODEL: {
			if (m_active_scene) {
				m_asset_loader->beginModelLoad(m_pending_gltf_path, true, m_pending_flip_v);
				m_async_op = PendingOp::ADD_MODEL;
			}
			break;
		}
		case PendingOp::NONE:
			break;
	}
	m_pending_op = PendingOp::NONE;
}

void SceneManager::tickAsyncLoader() {
	if (m_asset_loader->getState() == LoadState::IDLE ||
	    m_asset_loader->getState() == LoadState::FAILED)
		return;

	m_asset_loader->tick();

	if (m_asset_loader->getState() == LoadState::READY)
		finalizeAsyncLoad();
}

void SceneManager::finalizeAsyncLoad() {
	auto model = m_asset_loader->takeModel();
	if (!model)
		return;

	auto ctx = makeContext();
	if (m_async_op == PendingOp::LOAD_REGISTERED) {
		int idx = m_async_scene_index;
		if (idx >= 0 && idx < static_cast<int>(m_scene_entries.size())
		    && m_scene_entries[static_cast<size_t>(idx)].gltf_factory) {
			auto scene = m_scene_entries[static_cast<size_t>(idx)].gltf_factory(ctx, std::move(model));
			m_loaded_scene_index = idx;
			setActiveScene(std::move(scene));
		}
		m_async_scene_index = -1;
	} else if (m_async_op == PendingOp::ADD_MODEL && m_active_scene) {
		model->addToScene(m_active_scene->getRegistry(),
		                  {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
		m_event_bus.emitImmediate(AssetLoadCompleteEvent{
			m_asset_loader->getModelName(), {}});
	}
	m_async_op = PendingOp::NONE;
}

} // namespace ve