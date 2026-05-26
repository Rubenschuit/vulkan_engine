#include "pch.hpp"
#include "rendering/managers/scene_resource_manager.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "rendering/managers/bindless_texture_registry.hpp"
#include "rendering/managers/material_ssbo_manager.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "resources/ve_resource_manager.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"
#include "vulkan/ve_device.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

#include <algorithm>

namespace ve {

SceneResourceManager::SceneResourceManager(VeDevice& device, EventBus& event_bus, VeResourceManager& resource_manager)
	: m_ve_device(device),
	  m_event_bus(event_bus),
	  m_resource_manager(resource_manager),
	  m_mega_buffer(std::make_unique<PbrMegaBuffer>(device, event_bus)),
	  m_bindless_registry(std::make_unique<BindlessTextureRegistry>(device, event_bus)),
	  m_material_ssbo_manager(std::make_unique<MaterialSSBOManager>(device, *m_bindless_registry, event_bus)),
	  m_gpu_scene_manager(std::make_unique<GpuSceneManager>(device)) {}

SceneResourceManager::~SceneResourceManager() = default;

void SceneResourceManager::subscribeToEvents(EventBus& event_bus) {
	event_bus.subscribe<SceneLoadedEvent>([this](const SceneLoadedEvent& e) {
		loadScene(*e.registry);
	});
	event_bus.subscribe<SceneUnloadedEvent>([this](const SceneUnloadedEvent&) {
		unload();
	});
	event_bus.subscribe<AssetLoadCompleteEvent>([this](const AssetLoadCompleteEvent&) {
		if (auto* reg = m_gpu_scene_manager->getRegistry())
			rebuildForModelAdd(*reg);
	});
}

void SceneResourceManager::loadScene(Registry& registry) {
	auto meshes = collectUniqueMeshes(registry);
	if (!meshes.empty()) {
		auto cmd = m_ve_device.beginSingleTimeCommands();
		m_mega_buffer->build(*cmd, meshes);
		m_ve_device.endSingleTimeCommands(*cmd);
		for (VeMesh* mesh : meshes)
			mesh->releaseGpuBuffers();
	}

	m_gpu_scene_manager->subscribeToRegistry(registry);
	m_gpu_scene_manager->registerAllObjects(registry, *m_mega_buffer, *m_material_ssbo_manager);
}

void SceneResourceManager::unload() {
	m_ve_device.getDevice().waitIdle();
	m_mega_buffer->clear();
	m_gpu_scene_manager->reset();
	m_material_ssbo_manager->reset();
	m_bindless_registry->reset();
}

void SceneResourceManager::rebuildForModelAdd(Registry& registry) {
	auto meshes = collectUniqueMeshes(registry);
	if (meshes.empty())
		return;

	m_ve_device.getDevice().waitIdle();

	// new_mega.build copies from m_mega_buffer's GPU buffers for reused meshes.
	// endSingleTimeCommands waits on the fence, so the copies complete before
	// swapState moves those source buffers into new_mega
	PbrMegaBuffer new_mega(m_ve_device, m_event_bus);
	auto cmd = m_ve_device.beginSingleTimeCommands();
	new_mega.build(*cmd, meshes, m_mega_buffer.get());
	m_ve_device.endSingleTimeCommands(*cmd);

	m_mega_buffer->swapState(new_mega);

	m_gpu_scene_manager->reset();
	m_material_ssbo_manager->reset();
	m_bindless_registry->reset();
	m_resource_manager.flushPendingUnloads();

	m_gpu_scene_manager->subscribeToRegistry(registry);
	m_gpu_scene_manager->registerAllObjects(registry, *m_mega_buffer, *m_material_ssbo_manager);

	for (VeMesh* mesh : meshes)
		mesh->releaseGpuBuffers();
}

std::vector<VeMesh*> SceneResourceManager::collectUniqueMeshes(Registry& registry) {
	std::vector<VeMesh*> meshes;
	auto& mesh_pool = registry.meshes();
	for (uint32_t i = 0; i < mesh_pool.size(); i++) {
		VeMesh* m = mesh_pool.data()[i].getMesh();
		if (m && std::find(meshes.begin(), meshes.end(), m) == meshes.end())
			meshes.push_back(m);
	}
	return meshes;
}

} // namespace ve
