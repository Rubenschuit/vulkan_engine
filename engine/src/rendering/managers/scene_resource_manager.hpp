#pragma once
#include "ve_export.hpp"

#include <memory>
#include <vector>

namespace ve {

class VeDevice;
class Registry;
class VeMesh;
class PbrMegaBuffer;
class BindlessTextureRegistry;
class MaterialSSBOManager;
class GpuSceneManager;
class EventBus;
class VeResourceManager;

// Owns the scene-scoped GPU resource managers (mega buffer, bindless textures, material SSBO,
// GPU scene) and consolidates scene load / unload / model-add orchestration.
class VENGINE_API SceneResourceManager {
public:
	SceneResourceManager(VeDevice& device, EventBus& event_bus, VeResourceManager& resource_manager);
	~SceneResourceManager();

	SceneResourceManager(const SceneResourceManager&) = delete;
	SceneResourceManager& operator=(const SceneResourceManager&) = delete;

	void subscribeToEvents(EventBus& event_bus);

	// Build mega buffer, subscribe to registry events, register all GPU objects.
	void loadScene(Registry& registry);

	// Reset all managers.
	void unload();

	// Rebuild after a model is added to an existing scene.
	void rebuildForModelAdd(Registry& registry);

	PbrMegaBuffer& getMegaBuffer() { return *m_mega_buffer; }
	const PbrMegaBuffer& getMegaBuffer() const { return *m_mega_buffer; }
	BindlessTextureRegistry& getBindlessRegistry() { return *m_bindless_registry; }
	MaterialSSBOManager& getMaterialManager() { return *m_material_ssbo_manager; }
	GpuSceneManager& getGpuSceneManager() { return *m_gpu_scene_manager; }

private:
	static std::vector<VeMesh*> collectUniqueMeshes(Registry& registry);

	VeDevice& m_ve_device;
	EventBus& m_event_bus;
	VeResourceManager& m_resource_manager;
	std::unique_ptr<PbrMegaBuffer> m_mega_buffer;
	std::unique_ptr<BindlessTextureRegistry> m_bindless_registry;
	std::unique_ptr<MaterialSSBOManager> m_material_ssbo_manager;
	std::unique_ptr<GpuSceneManager> m_gpu_scene_manager;
};

} // namespace ve
