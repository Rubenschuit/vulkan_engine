#include "pch.hpp"
#include "rendering/managers/scene_resource_manager.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "rendering/managers/bindless_texture_registry.hpp"
#include "rendering/managers/material_ssbo_manager.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"
#include "vulkan/ve_device.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

#include <algorithm>

namespace ve {

SceneResourceManager::SceneResourceManager(VeDevice& device)
	: m_ve_device(device),
	  m_mega_buffer(std::make_unique<PbrMegaBuffer>(device)),
	  m_bindless_registry(std::make_unique<BindlessTextureRegistry>(device)),
	  m_material_ssbo_manager(std::make_unique<MaterialSSBOManager>(device, *m_bindless_registry)),
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
	m_mega_buffer->clear();
	m_gpu_scene_manager->reset();
	m_material_ssbo_manager->reset();
	m_bindless_registry->reset();

	auto cmd = m_ve_device.beginSingleTimeCommands();
	m_mega_buffer->build(*cmd, meshes);
	m_ve_device.endSingleTimeCommands(*cmd);

	m_gpu_scene_manager->subscribeToRegistry(registry);
	m_gpu_scene_manager->registerAllObjects(registry, *m_mega_buffer, *m_material_ssbo_manager);
}

std::vector<VeMesh*> SceneResourceManager::collectUniqueMeshes(Registry& registry) {
	std::vector<VeMesh*> meshes;
	for (auto& mc : registry.meshes()) {
		VeMesh* m = mc.getMesh();
		if (m && std::find(meshes.begin(), meshes.end(), m) == meshes.end())
			meshes.push_back(m);
	}
	return meshes;
}

} // namespace ve
