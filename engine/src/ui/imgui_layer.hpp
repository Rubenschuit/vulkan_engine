#pragma once
#include "ve_export.hpp"
#include "game/ve_frame_info.hpp"
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include "systems/particle_system.hpp"
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

// Minimal Dear ImGui integration header. Implementation in imgui_layer.cpp
// This uses glfw + Vulkan backends. Assumes VkInstance/Device/Queue/Swapchain are managed elsewhere.

namespace ve {

class VeWindow;
class VeDevice;
class VeRenderer;

struct VENGINE_API UIContext {
	// general
	bool visible = false;
	bool show_performance = true;
	bool show_controls = true;
	bool show_axes = false;
	int current_scene = 1;
	RenderMode render_mode = RenderMode::BRDF_MICROFACET;

	// sponza settings
	float sun_intensity = 2000.0f;

	// graphics settings
	int shadow_mode = ShadowMode::REGULAR;
	int topology = Topology::TRIANGLE_LIST;
	bool hdr_enabled = false;
	bool msaa = true;
	bool vsync = false;

	// particle system
	int current_mode = 1;
	float speed = 1.0f;

	// particle count
	uint32_t pending_particle_count = 10000;
	bool apply_particle_count = false;
	bool reset_particle_count = false;

	// particle explosion normal dist
	float particle_velocity_mean = 0.0f;
	float particle_velocity_stddev = 1.0f;
	bool apply_velocity_params = false;

	// lifetime
	float min_life = 1.0f;
	float max_life = 3.0f;
	bool should_respawn = true;

	// emission
	bool emit_burst = false;
	int emit_count = 1000;

	// post process
	int blur_radius = 0;
	float blur_strength = 1.0f;
	float exposure = 1.0f;
	bool bloom_enabled = true;
	float bloom_strength = 0.01f;
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

    // Render engine-specific windows (Settings, Performance).
    // Should be called between beginFrame() and endFrame().
    void renderEngineWindows(UIContext& context);

    void recreatePipeline();

private:
    void uploadFonts();

    VeDevice& m_device;
    VeRenderer& m_renderer;
    VkDescriptorPool m_descriptor_pool = VK_NULL_HANDLE;
    VkFormat m_color_format = VK_FORMAT_UNDEFINED;
};
}
