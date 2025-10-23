#pragma once
#include "ve_export.hpp"

// Minimal Dear ImGui integration header. Implementation in imgui_layer.cpp
// This uses glfw + Vulkan backends. Assumes VkInstance/Device/Queue/Swapchain are managed elsewhere.

namespace ve {

class VeWindow;
class VeDevice;
class VeRenderer;

struct VENGINE_API UIContext {
	// general
	bool visible;

	// particle count
	uint32_t pending_particle_count;
	bool apply_particle_count;
	bool reset_particle_count;

	// particle velocity normal dist
	float particle_velocity_mean;
	float particle_velocity_stddev;
	bool apply_velocity_params;

};

class VENGINE_API ImGuiLayer {
public:
    ImGuiLayer(VeWindow& window, VeDevice& device, VeRenderer& renderer);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Call once per frame before your rendering
    void beginFrame();
    // Record UI draw data into the active command buffer
    void endFrame(vk::raii::CommandBuffer& cmd);

    // High-level helper: draw UI and submit, capturing panel intents.
    // - ui_visible: when false, no window is drawn.
    // - pending_particle_count: in/out staged value edited by the UI.
    // - out_apply/out_reset: set to true when the user presses the corresponding buttons.
    void renderUI(UIContext& context);

private:
    void uploadFonts();

    VeDevice& m_device;
    VeRenderer& m_renderer;
    VkDescriptorPool m_descriptor_pool = VK_NULL_HANDLE;
    VkFormat m_color_format = VK_FORMAT_UNDEFINED;
};
}
