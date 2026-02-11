#pragma once
#include "ve_export.hpp"
#include "scene/ve_camera.hpp"
#include "scene/ve_game_object.hpp"
#include "rendering/ve_frame_info.hpp"
#include <unordered_map>

namespace ve {

class VENGINE_API CullingSystem {
public:
	explicit CullingSystem(VeCamera& camera);
	void setCamera(VeCamera& camera) { m_camera = &camera; }
	// Cull objects in the scene and stores the references to the visible objects in the frame info.
	void cullObjects(VeFrameInfo& frame_info);

	std::unordered_map<uint32_t, VeGameObject*>& getVisibleGameObjectsRef() { return m_visible_game_objects; }

	uint32_t getLastTotalMeshObjects() const { return m_last_total_mesh_objects; }
	uint32_t getLastVisibleCount() const { return m_last_visible_count; }

private:
	VeCamera* m_camera;
	std::unordered_map<uint32_t, VeGameObject*> m_visible_game_objects;
	uint32_t m_last_total_mesh_objects = 0;
	uint32_t m_last_visible_count = 0;
};
} // namespace ve