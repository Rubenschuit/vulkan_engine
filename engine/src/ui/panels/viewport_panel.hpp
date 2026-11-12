#pragma once
#include "ui/editor_panel.hpp"
#include "ui/editor_state.hpp"
#include <imgui.h>
#include <vulkan/vulkan.h>

namespace ve {

struct CameraView;
struct Ray;
class FlyCameraController;
class PhysicsSystem;
class EventBus;

class VENGINE_API ViewportPanel : public EditorPanel {
public:
	void setTextureID(VkDescriptorSet texture_id) { m_texture_id = texture_id; }
	void setCameraView(const CameraView* camera_view) { m_camera_view = camera_view; }
	void setPhysicsSystem(PhysicsSystem* ps) { m_physics_system = ps; }
	void setEventBus(EventBus* bus) { m_event_bus = bus; }

	// The editor camera, whose speed and mouse sensitivity the toolbar edits and imgui.ini
	// keeps. Call before the first UI frame, which is when ImGui reads the ini.
	void setFlyCamera(FlyCameraController* camera);

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Viewport"; }

	ImVec2 getImageMin() const { return m_image_min; }
	ImVec2 getImageMax() const { return m_image_max; }
	bool isImageValid() const { return m_image_max.x > m_image_min.x && m_image_max.y > m_image_min.y; }
	void invalidateImageRect() { m_image_min = ImVec2(0.f, 0.f); m_image_max = ImVec2(0.f, 0.f); }

private:
	void renderGizmoToolbar(EditorState& state);
	void renderCameraSelector(Registry* registry, EditorState& state);
	void renderCameraSettings();
	void renderGizmo(Registry* registry, EditorState& state, float img_x, float img_y, float img_w, float img_h);
	void renderCollisionShape(Registry* registry, EditorState& state, float img_x, float img_y, float img_w, float img_h);
	void handleRightMouse(Registry* registry, EditorState& state);
	void renderContextMenu(Registry* registry, EditorState& state);
	void renderSpeedReadout();
	bool rayUnderMouse(Ray& ray) const;

	VkDescriptorSet m_texture_id = VK_NULL_HANDLE;
	const CameraView* m_camera_view = nullptr;
	PhysicsSystem* m_physics_system = nullptr;
	EventBus* m_event_bus = nullptr;
	FlyCameraController* m_fly_camera = nullptr;
	std::vector<Entity> m_frozen_entities;
	bool m_was_gizmo_active = false;
	static constexpr float GIZMO_MIN_SCALE_FACTOR = 0.05f;
	glm::vec3 m_drag_start_axis_lengths{1.0f};
	ImVec2 m_image_min{0.f, 0.f};
	ImVec2 m_image_max{0.f, 0.f};

	// Right button pressed over the image, not yet a click (menu) or a drag (mouse look)
	bool m_right_pending = false;
	glm::vec3 m_menu_point{0.0f};
	glm::vec3 m_menu_normal{0.0f, 0.0f, 1.0f};
	double m_speed_readout_until = 0.0;
};

}
