#include "pch.hpp"
#include "rendering/scene_resource_manager.hpp"
#include "rendering/bindless_texture_registry.hpp"
#include "rendering/material_ssbo_manager.hpp"
#include "rendering/gpu_scene_manager.hpp"
#include "rendering/gpu_culling_system.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "rendering/pbr_render_system.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"
#include "vulkan/ve_device.hpp"

#include <algorithm>

namespace ve {

SceneResourceManager::SceneResourceManager(VeDevice& device)
	: m_ve_device(device),
	  m_bindless_registry(std::make_unique<BindlessTextureRegistry>(device)),
	  m_material_ssbo_manager(std::make_unique<MaterialSSBOManager>(device, *m_bindless_registry)),
	  m_gpu_scene_manager(std::make_unique<GpuSceneManager>(device)) {}

SceneResourceManager::~SceneResourceManager() = default;

void SceneResourceManager::loadScene(Registry& registry, PbrRenderSystem& pbr_system) {
	auto meshes = collectUniqueMeshes(registry);
	if (!meshes.empty()) {
		auto cmd = m_ve_device.beginSingleTimeCommands();
		pbr_system.buildMegaBuffer(*cmd, meshes);
		m_ve_device.endSingleTimeCommands(*cmd);
	}

	m_gpu_scene_manager->subscribeToRegistry(registry);
	m_gpu_scene_manager->registerAllObjects(
		registry, pbr_system.getMegaBuffer(), *m_material_ssbo_manager);
}

void SceneResourceManager::unload(PbrRenderSystem& pbr_system, GpuCullingSystem& gpu_culling) {
	m_ve_device.getDevice().waitIdle();
	pbr_system.resetMegaBuffer();
	m_gpu_scene_manager->reset();
	gpu_culling.clearReadback();
	m_material_ssbo_manager->reset();
	m_bindless_registry->reset();
}

void SceneResourceManager::rebuildForModelAdd(Registry& registry, PbrRenderSystem& pbr_system) {
	auto meshes = collectUniqueMeshes(registry);
	if (meshes.empty())
		return;

	m_ve_device.getDevice().waitIdle();
	pbr_system.resetMegaBuffer();
	m_gpu_scene_manager->reset();
	m_material_ssbo_manager->reset();
	m_bindless_registry->reset();

	auto cmd = m_ve_device.beginSingleTimeCommands();
	pbr_system.buildMegaBuffer(*cmd, meshes);
	m_ve_device.endSingleTimeCommands(*cmd);

	m_gpu_scene_manager->subscribeToRegistry(registry);
	m_gpu_scene_manager->registerAllObjects(
		registry, pbr_system.getMegaBuffer(), *m_material_ssbo_manager);
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
