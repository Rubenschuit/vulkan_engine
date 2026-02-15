#pragma once
#include "ve_export.hpp"
#include "scene/ve_camera.hpp"
#include "rendering/ve_frame_info.hpp"
#include <vector>

namespace ve {

class VENGINE_API CullingSystem {
public:
	explicit CullingSystem(VeCamera& camera);
	void setCamera(VeCamera& camera) { m_camera = &camera; }
	void setCullingEnabled(bool enabled) { m_culling_enabled = enabled; }
	bool isCullingEnabled() const { return m_culling_enabled; }
	void cullObjects(VeFrameInfo& frame_info);

	std::vector<VisibleObject>& getVisibleObjectsRef() { return m_visible_objects; }

	uint32_t getLastTotalMeshObjects() const { return m_last_total_mesh_objects; }
	uint32_t getLastVisibleCount() const { return m_last_visible_count; }

private:
	VeCamera* m_camera;
	bool m_culling_enabled = true;
	std::vector<VisibleObject> m_visible_objects;
	uint32_t m_last_total_mesh_objects = 0;
	uint32_t m_last_visible_count = 0;
};
} // namespace ve