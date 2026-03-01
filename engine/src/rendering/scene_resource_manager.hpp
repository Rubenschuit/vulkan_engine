#pragma once
#include "ve_export.hpp"

#include <memory>
#include <vector>

namespace ve {

class VeDevice;
class Registry;
class VeMesh;
class PbrRenderSystem;
class GpuCullingSystem;
class BindlessTextureRegistry;
class MaterialSSBOManager;
class GpuSceneManager;

// Owns the three scene-scoped GPU resource managers (bindless textures, material SSBO,
// GPU scene) and consolidates scene load / unload / model-add orchestration.
class VENGINE_API SceneResourceManager {
public:
	SceneResourceManager(VeDevice& device);
	~SceneResourceManager();

	SceneResourceManager(const SceneResourceManager&) = delete;
	SceneResourceManager& operator=(const SceneResourceManager&) = delete;

	// Build mega buffer, subscribe to registry events, register all GPU objects.
	void loadScene(Registry& registry, PbrRenderSystem& pbr_system);

	// Full teardown: reset all managers and clear GPU culling readback.
	void unload(PbrRenderSystem& pbr_system, GpuCullingSystem& gpu_culling);

	// Rebuild after a model is added to an existing scene (no culling readback clear).
	void rebuildForModelAdd(Registry& registry, PbrRenderSystem& pbr_system);

	BindlessTextureRegistry& getBindlessRegistry() { return *m_bindless_registry; }
	MaterialSSBOManager& getMaterialManager() { return *m_material_ssbo_manager; }
	GpuSceneManager& getGpuSceneManager() { return *m_gpu_scene_manager; }

private:
	static std::vector<VeMesh*> collectUniqueMeshes(Registry& registry);

	VeDevice& m_ve_device;
	std::unique_ptr<BindlessTextureRegistry> m_bindless_registry;
	std::unique_ptr<MaterialSSBOManager> m_material_ssbo_manager;
	std::unique_ptr<GpuSceneManager> m_gpu_scene_manager;
};

} // namespace ve
