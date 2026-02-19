#pragma once
#include "ve_export.hpp"
#include "rendering/ve_frame_info.hpp"
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include "rendering/particle_system.hpp"
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <functional>

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
	bool show_aabb_debug = false;

	// graphics settings
	float fov = 80.0f;
	ShadowMode shadow_mode = ShadowMode::PCF;
	float pcss_light_size = 0.04f;
	int csm_blend_mode = 2; // 0=off, 1=linear, 2=dithered TODO: enum
	Topology topology = Topology::TRIANGLE_LIST;
	bool hdr_enabled = false;
	bool msaa = false;
	bool vsync = false;

	// timing
	float cpu_time = 0.0f;
	float gpu_time = 0.0f;
	float compute_gpu_time = 0.0f;
	float gpu_overlap = 0.0f;

	// particle systems
	bool particles_enabled = true;
	bool fireworks_enabled = true;

	// depth pre-pass
	bool depth_prepass_enabled = true;

	// culling
	bool enable_frustum_culling = true;
	uint32_t cull_total_objects = 0;
	uint32_t cull_visible_objects = 0;

	// post process
	int blur_radius = 0;
	float blur_strength = 1.0f;
	float exposure = 1.0f;
	int tone_map_mode = TONEMAP_NONE;
	float hdr_peak_white = 4.0f;
	bool bloom_enabled = true;
	float bloom_strength = 0.01f;

	virtual ~UIContext() = default;
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

	// Render the UI. If appUiCallback is provided, it will be called after the engine windows are rendered.
	void renderUI(UIContext& context, std::function<void(UIContext&)> appUiCallback = nullptr);

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
