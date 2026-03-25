#pragma once
#include "ve_export.hpp"
#include "rendering/ve_frame_info.hpp"
#include "rendering/particle_system.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <functional>

namespace ve {

class VeWindow;
class VeDevice;
class VeRenderer;
struct EditorState;

struct VENGINE_API UIContext {
	// general
	bool visible = false;
	bool show_performance = true;
	bool show_controls = true;
	bool show_axes = false;
	bool show_aabb_debug = false;

	// graphics settings
	float fov = 75.0f;
	ShadowMode shadow_mode = ShadowMode::PCF;
	float pcss_light_size = 0.04f;
	int pcf_samples = 8; // multiples of 4 between 4 and 64, or 0 to disable PCF
	int pcss_filter_samples = 16;
	int csm_blend_mode = 2; // 0=off, 1=linear, 2=dithered TODO: enum
	float shadow_bias = ve::SHADOW_BIAS;
	float csm_normal_bias = ve::CSM_NORMAL_BIAS;
	Topology topology = Topology::TRIANGLE_LIST;
	RenderMode render_mode = RenderMode::BRDF_MICROFACET;
	bool hdr_enabled = false;
	bool msaa = false;
	bool vsync = false;

	// lighting
	glm::vec3 ambient_light_color = glm::vec3(1.0f);
	float ambient_light_intensity = 0.006f;
	bool cluster_enabled = true;
	bool ibl_enabled = true;
	float ibl_diffuse_intensity = 0.2f;
	float ibl_specular_intensity = 0.2f;
	float ibl_min_ambient = 0.005f;
	bool ibl_auto_exposure = true;
	float ibl_exposure_compensation = 1.0f; // read-only, written by application

	// read-only stats (engine writes, UI reads)
	struct Stats {
		float cpu_time = 0.0f;
		float fence_wait = 0.0f;
		float acquire_wait = 0.0f;
		float gpu_time = 0.0f;
		float compute_gpu_time = 0.0f;
		float gpu_overlap = 0.0f;

		// Per-system GPU breakdown
		float gpu_shadow_maps = 0.0f;
		float gpu_depth_prepass = 0.0f;
		float gpu_gtao = 0.0f;
		float gpu_scene_render = 0.0f;
		float gpu_bloom = 0.0f;
		float gpu_post_process = 0.0f;

		// Per-system CPU breakdown
		float cpu_culling = 0.0f;
		float cpu_shadow_maps = 0.0f;
		float cpu_depth_prepass = 0.0f;
		float cpu_gtao = 0.0f;
		float cpu_scene_render = 0.0f;
		float cpu_bloom = 0.0f;
		float cpu_post_process = 0.0f;
		float cpu_physics = 0.0f;

		uint32_t cull_total_objects = 0;
		uint32_t cull_visible_objects = 0;
		uint32_t visible_triangles = 0;
		uint32_t draw_calls = 0;
		uint32_t transparent_draw_calls = 0;
		uint32_t num_point_lights = 0;
		uint32_t num_directional_lights = 0;
	};
	Stats stats;

	// depth pre-pass
	bool depth_prepass_enabled = true;

	// screen-space shadow mask (async compute)
	bool shadow_mask_enabled = true;
	bool shadow_mask_half_res = false;

	// GTAO (screen-space ambient occlusion)
	bool gtao_enabled = true;
	bool gtao_half_res = true;
	float gtao_radius = 0.25f;
	float gtao_intensity = 0.5f;

	// culling
	bool enable_frustum_culling = true;
	bool gpu_culling_enabled = false;
	bool hiz_occlusion_enabled = false;
	bool meshlet_culling_enabled = false;
	bool meshlet_gpu_shadow_fallback = false;

	// physics
	bool physics_enabled = true;

	// GPU profiling
	bool gpu_profiling = false;

	// multi-threading thresholds
	int min_parallel_cull_entities = static_cast<int>(ve::MIN_PARALLEL_CULL_ENTITIES);

	// LOD override
	int lod_force_level = -1;  // -1 = auto (normal LOD selection), 0..3 = force specific LOD
	float lod_screen_thresholds[3] = {0.3f, 0.15f, 0.05f};
	float lod_hysteresis = 0.2f;

	// post process
	int blur_radius = 0;
	float blur_strength = 1.0f;
	float exposure = 1.0f;
	int tone_map_mode = TONEMAP_GT;
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
    // Record UI draw data into the active command buffer.
    // When clear_target is true, the swapchain attachment uses eClear (editor mode, swapchain is fresh).
    void endFrame(vk::raii::CommandBuffer& cmd, bool clear_target = false);

	// Render the UI. appUiCallback renders all panels and app-specific windows.
	void renderUI(UIContext& context, EditorState& editor_state,
				  std::function<void(UIContext&)> appUiCallback = nullptr);

	// Viewport image registration for render-to-texture
	void registerViewportImage(VkSampler sampler, VkImageView image_view, VkImageLayout layout);
	void unregisterViewportImage();
	VkDescriptorSet getViewportTextureId() const { return m_viewport_texture_id; }

    void recreatePipeline();

	// Set the app-specific settings window name
	void setAppSettingsWindowName(const std::string& name) { m_app_settings_window_name = name; }
	const std::string& getAppSettingsWindowName() const { return m_app_settings_window_name; }

private:
	void renderDockSpace();
	void applyEditorTheme();
    void uploadFonts();

    VeDevice& m_device;
    VeRenderer& m_renderer;
    VkDescriptorPool m_descriptor_pool = VK_NULL_HANDLE;
    VkFormat m_color_format = VK_FORMAT_UNDEFINED;
	VkDescriptorSet m_viewport_texture_id = VK_NULL_HANDLE;
	std::string m_app_settings_window_name = "Settings";
};
}
