#include "pch.hpp"
#include "ui/panels/graphics_panel.hpp"
#include "ui/editor_state.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/imgui_helpers.hpp"
#include "platform/ve_window.hpp"
#include "rendering/particle_backend.hpp"
#include "rendering/ve_renderer.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <string>
#include <vector>

namespace ve {

using namespace ve::ui;

namespace {

// Slider on a held copy that writes the setting once on release
template <typename T, typename Slider>
void sliderAppliedOnRelease(const char* label, T& setting, T& held, Slider slider) {
	if (ImGui::GetActiveID() != ImGui::GetID(label))
		held = setting;
	slider(label, &held);
	if (ImGui::IsItemDeactivatedAfterEdit())
		setting = held;
}

}

void GraphicsPanel::render(Registry* /*registry*/, EditorState& state, UIContext& ctx) {
	if (!ImGui::Begin("Graphics", &state.show_settings, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	// --- Display ---
	if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("FOV (deg)", &ctx.settings.fov, 30.0f, 120.0f, "%.1f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Vertical FOV of the Editor Camera; scene cameras use their own.");

		{
			static const char* mode_labels[] = {"Windowed", "Borderless", "Fullscreen"};
			VeWindow::WindowMode current_mode = m_window.getWindowMode();
			int mode_idx = static_cast<int>(current_mode);
			if (ImGui::Combo("Window Mode", &mode_idx, mode_labels, IM_ARRAYSIZE(mode_labels))) {
				m_window.setWindowMode(static_cast<VeWindow::WindowMode>(mode_idx));
				current_mode = m_window.getWindowMode();
			}

			if (current_mode != VeWindow::WindowMode::Windowed) {
				auto monitors = m_window.getMonitors();
				if (monitors.size() > 1) {
					int sel = m_window.getResolvedMonitorIndex();
					auto label = [](const VeWindow::MonitorInfo& m) {
						return std::to_string(m.index) + ": " + m.name + " ("
							+ std::to_string(m.width) + "x" + std::to_string(m.height)
							+ " @" + std::to_string(m.refresh_rate) + "Hz)";
					};
					std::string preview = label(monitors[static_cast<size_t>(sel)]);
					if (ImGui::BeginCombo("Monitor", preview.c_str())) {
						for (const auto& m : monitors) {
							bool is_selected = (m.index == sel);
							if (ImGui::Selectable(label(m).c_str(), is_selected))
								m_window.setMonitor(m.index);
							if (is_selected)
								ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
				}
			}

			if (current_mode == VeWindow::WindowMode::Fullscreen) {
				auto modes = m_window.getVideoModes(m_window.getResolvedMonitorIndex());
				if (!modes.empty()) {
					VeWindow::VideoMode current = m_window.getTargetVideoMode();
					auto label = [](const VeWindow::VideoMode& m) {
						return std::to_string(m.width) + "x" + std::to_string(m.height)
							+ " @ " + std::to_string(m.refresh_rate) + "Hz";
					};
					std::string preview = (current.width > 0) ? label(current) : "Desktop default";
					if (ImGui::BeginCombo("Video Mode", preview.c_str())) {
						bool desktop_selected = (current.width == 0);
						if (ImGui::Selectable("Desktop default", desktop_selected))
							m_window.setVideoMode({0, 0, 0});
						if (desktop_selected)
							ImGui::SetItemDefaultFocus();
						for (const auto& m : modes) {
							bool is_selected = (m == current);
							if (ImGui::Selectable(label(m).c_str(), is_selected))
								m_window.setVideoMode(m);
							if (is_selected)
								ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
				}
			}

			GLFWwindow* gw = m_window.getGLFWwindow();
			GLFWmonitor* fs_mon = glfwGetWindowMonitor(gw);
			bool decorated = glfwGetWindowAttrib(gw, GLFW_DECORATED) != 0;
			int wx = 0, wy = 0, ww = 0, wh = 0;
			glfwGetWindowPos(gw, &wx, &wy);
			glfwGetWindowSize(gw, &ww, &wh);
			const char* glfw_state = fs_mon ? "GLFW fullscreen" : (decorated ? "GLFW windowed" : "GLFW windowed (undecorated)");
			ImGui::TextDisabled("%s  pos=(%d,%d)  size=%dx%d", glfw_state, wx, wy, ww, wh);
		}

		if (ImGui::Checkbox("Enable VSync", &ctx.settings.vsync))
			m_renderer.setVSync(ctx.settings.vsync);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Present mode: FIFO when on, IMMEDIATE when off\n(FIFO if IMMEDIATE is unavailable).");

		bool hdr_supported = m_renderer.hasHdrSupport();
		if (!hdr_supported)
			ImGui::BeginDisabled();
		if (ImGui::Checkbox("Enable HDR", &ctx.settings.hdr_enabled))
			m_renderer.setHdrEnabled(ctx.settings.hdr_enabled);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Swapchain color space: HDR10 PQ, or Extended sRGB Linear on macOS\n(the other if unavailable). SDR if the surface offers neither.");
		if (hdr_supported && ctx.settings.hdr_enabled) {
			const char* mode_str = m_renderer.getHDRColorModeString();
			if (mode_str[0] != '\0') {
				ImGui::SameLine();
				ImGui::TextDisabled("| Selected Mode: %s", mode_str);
			}
		}
		if (!hdr_supported) {
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("(Not supported by device)");
		}

		ImGui::Separator();
		auto extent = m_renderer.getExtent();
		ImGui::Text("Resolution: %d x %d", extent.width, extent.height);
	}

	// --- Culling ---
	if (ImGui::CollapsingHeader("Culling", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Checkbox("Geometry Pre-pass", &ctx.settings.geometry_prepass_enabled))
			m_event_bus.emitImmediate(GeometryPrePassChangedEvent{ctx.settings.geometry_prepass_enabled});
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Early depth + normal/roughness G-buffer pass.\nRequired by GTAO, shadow mask, SSR, Radiance Cascades, and Hi-Z occlusion culling.");
		ImGui::Checkbox("Frustum Culling", &ctx.settings.enable_frustum_culling);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("CPU backend only; GPU and Meshlet always frustum cull.");
		ImGui::Text("Culling Backend");
		int backend = static_cast<int>(ctx.settings.culling_backend);
		ImGui::RadioButton("CPU", &backend, static_cast<int>(CullingBackendMode::CPU));
		ImGui::RadioButton("GPU", &backend, static_cast<int>(CullingBackendMode::GPU));
		ImGui::RadioButton("Meshlet", &backend, static_cast<int>(CullingBackendMode::MESHLET));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Sub-object meshlet culling (frustum + backface cone + Hi-Z).");
		ctx.settings.culling_backend = static_cast<CullingBackendMode>(backend);

		ImGui::BeginDisabled(ctx.settings.culling_backend == CullingBackendMode::CPU);
		ImGui::Checkbox("Hi-Z Occlusion Culling", &ctx.settings.hiz_occlusion_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Skips objects occluded by the depth buffer.\nRequires geometry pre-pass and a GPU/Meshlet backend.");
		ImGui::EndDisabled();

		if (ctx.settings.culling_backend == CullingBackendMode::MESHLET) {
			ImGui::Indent();
			ImGui::Checkbox("Object Culled Shadows", &ctx.settings.meshlet_object_culled_shadows);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Also spin up the GPU culling system and use its object-level shadow path.\nCosts extra VRAM.");
			ImGui::Unindent();
		}
		ImGui::SeparatorText("Multi-threading");
		ImGui::SliderInt("Min Cull Entities", &ctx.settings.min_parallel_cull_entities, 0, 4096);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Minimum mesh entities to enable parallel cpu frustum culling.\n0 = always parallel");
	}

	// --- Anti-Aliasing ---
	if (ImGui::CollapsingHeader("Anti-Aliasing")) {
		auto available = m_renderer.getAvailableSampleCounts();
		int current = m_renderer.getCurrentSampleCountInt();

		std::vector<std::string> sample_labels;
		size_t current_index = 0;
		for (size_t i = 0; i < available.size(); i++) {
			sample_labels.push_back(available[i] == 1 ? "Off" : std::to_string(available[i]) + "x");
			if (available[i] == current)
				current_index = i;
		}

		ImGui::Text("MSAA:");
		int slider_value = static_cast<int>(current_index);
		ImGui::PushItemWidth(200.0f);
		if (ImGui::SliderInt("##msaa_slider", &slider_value, 0, static_cast<int>(available.size()) - 1, "")) {
			slider_value = std::clamp(slider_value, 0, static_cast<int>(available.size()) - 1);
			m_renderer.setSampleCountInt(available[static_cast<size_t>(slider_value)]);
		}
		ImGui::PopItemWidth();
		ImGui::SameLine();
		ImGui::Text("%s", sample_labels[current_index].c_str());
	}

	// --- Shadows ---
	if (ImGui::CollapsingHeader("Shadows")) {
		ImGui::PushItemWidth(200.0f);
		int shadow_mode_int = static_cast<int>(ctx.settings.shadow_mode);
		if (ImGui::SliderInt("##shadow_slider", &shadow_mode_int, 0, 3, ""))
			ctx.settings.shadow_mode = static_cast<ShadowMode>(std::clamp(shadow_mode_int, 0, 3));
		ImGui::PopItemWidth();
		ImGui::SameLine();
		const char* shadow_labels[] = {"Off", "Normal", "PCF", "PCSS"};
		ImGui::Text("%s", shadow_labels[static_cast<uint32_t>(ctx.settings.shadow_mode)]);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::Text("Normal: Hard shadows");
			ImGui::Text("PCF: Percentage Closer Filtering");
			ImGui::TextColored(COL_MUTED, "     Soft shadow edges");
			ImGui::Text("PCSS: Percentage Closer Soft Shadows");
			ImGui::TextColored(COL_MUTED, "     Contact-hardening soft shadows");
			ImGui::EndTooltip();
		}
		if (ctx.settings.shadow_mode != ShadowMode::DISABLED) {
			int preset_idx = static_cast<int>(ctx.settings.shadow_resolution_preset);
			if (ImGui::Combo("Shadow Resolution", &preset_idx, "Low\0" "Medium\0" "High\0" "Ultra\0"))
				ctx.settings.shadow_resolution_preset = static_cast<ShadowResolutionPreset>(
					std::clamp(preset_idx, 0, static_cast<int>(SHADOW_RESOLUTION_PRESET_COUNT) - 1));
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::Text("Shadow Atlas Resolution");
				ImGui::Separator();
				for (uint32_t i = 0; i < SHADOW_RESOLUTION_PRESET_COUNT; i++) {
					const auto& v = SHADOW_RESOLUTION_PRESETS[i];
					const char* names[] = {"Low", "Medium", "High", "Ultra"};
					ImGui::Text("%s: CSM %u/%u/%u  Spot/Point %u",
						names[i], v.csm[0], v.csm[1], v.csm[2], v.point);
				}
				ImGui::TextColored(COL_MUTED,
					"Changing rebuilds the atlas.");
				ImGui::EndTooltip();
			}
		}
		if (ctx.settings.shadow_mode == ShadowMode::PCSS) {
			ImGui::SliderFloat("Light Size", &ctx.settings.pcss_light_size, 0.001f, 0.2f, "%.3f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Virtual light size for PCSS penumbra.\nLarger = softer shadows farther from caster.");
		}
		if (ctx.settings.shadow_mode == ShadowMode::PCF || ctx.settings.shadow_mode == ShadowMode::PCSS) {
			static constexpr int pcf_values[] = {4, 8, 16, 32};
			int pcf_idx = 0;
			for (int j = 0; j < 4; j++)
				if (ctx.settings.pcf_samples == pcf_values[j])
					pcf_idx = j;
			if (ImGui::Combo("PCF Samples", &pcf_idx, "4\0" "8\0" "16\0" "32\0"))
				ctx.settings.pcf_samples = pcf_values[pcf_idx];
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Poisson disk samples for PCF filtering.\nAlso used for PCSS blocker search.");
		}
		if (ctx.settings.shadow_mode == ShadowMode::PCSS) {
			static constexpr int pcss_values[] = {8, 16, 32};
			int pcss_idx = 0;
			for (int j = 0; j < 3; j++)
				if (ctx.settings.pcss_filter_samples == pcss_values[j])
					pcss_idx = j;
			if (ImGui::Combo("PCSS Filter Samples", &pcss_idx, "8\0" "16\0" "32\0"))
				ctx.settings.pcss_filter_samples = pcss_values[pcss_idx];
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Poisson disk samples for PCSS variable-radius filter.");
		}
		if (ctx.settings.shadow_mode != ShadowMode::DISABLED) {
			ImGui::SliderFloat("Shadow Bias", &ctx.settings.shadow_bias, 0.0f, 0.01f, "%.5f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Depth comparison bias.\nHigher = less acne but more Peter panning.");
			ImGui::SliderFloat("Normal Bias", &ctx.settings.csm_normal_bias, 0.0f, 8.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("CSM normal offset in shadow texels (auto-scaled per cascade and by\nangle to the light). Pushes the shadow lookup along the surface normal.");
			ImGui::SliderFloat("Depth Bias Constant", &ctx.settings.depth_bias_constant, 0.0f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rasterizer constant depth bias applied when rendering shadow maps.\nHigher = less acne but more Peter panning.");
			ImGui::SliderFloat("Depth Bias Slope", &ctx.settings.depth_bias_slope, 0.0f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rasterizer slope-scaled depth bias applied when rendering shadow maps.\nScales with surface slope relative to light direction.");
			ImGui::SliderFloat("Depth Bias Clamp", &ctx.settings.depth_bias_clamp, 0.0f, 0.1f, "%.4f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Maximum absolute depth bias. 0 = unclamped.");
			int cull_idx = static_cast<int>(ctx.settings.shadow_cull_mode);
			ImGui::Text("Shadow Cull:");
			ImGui::SameLine();
			ImGui::RadioButton("Front##scull", &cull_idx, 0);
			ImGui::SameLine();
			ImGui::RadioButton("Back##scull", &cull_idx, 1);
			ImGui::SameLine();
			ImGui::RadioButton("None##scull", &cull_idx, 2);
			ctx.settings.shadow_cull_mode = static_cast<ShadowCullMode>(cull_idx);
			ImGui::Text("CSM Blend: ");
			ImGui::SameLine();
			ImGui::RadioButton("Off", &ctx.settings.csm_blend_mode, 0);
			ImGui::SameLine();
			ImGui::RadioButton("Linear", &ctx.settings.csm_blend_mode, 1);
			ImGui::SameLine();
			ImGui::RadioButton("Dithered", &ctx.settings.csm_blend_mode, 2);
			ImGui::Checkbox("Shadow Mask (async compute)", &ctx.settings.shadow_mask_enabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Evaluates CSM shadows once per pixel.\nRequires geometry pre-pass enabled.");
			if (ctx.settings.shadow_mask_enabled) {
				ImGui::Indent();
				ImGui::Checkbox("Half Resolution", &ctx.settings.shadow_mask_half_res);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("4x fewer compute invocations, depth-aware upsampled.");
				ImGui::Unindent();
			}
		}
	}

	// --- Ambient Occlusion ---
	if (ImGui::CollapsingHeader("Ambient Occlusion")) {
		ImGui::Checkbox("GTAO", &ctx.settings.gtao_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Requires geometry pre-pass enabled.");
		if (ctx.settings.gtao_enabled) {
			ImGui::Indent();
			ImGui::Checkbox("Half Resolution##gtao", &ctx.settings.gtao_half_res);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("4x fewer compute invocations, bilinear upsampled.");
			bool radius_changed = ImGui::SliderFloat("AO Radius", &ctx.settings.gtao_radius, 0.1f, 3.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("World-space sampling radius for AO.");
			bool intensity_changed = ImGui::SliderFloat("AO Intensity", &ctx.settings.gtao_intensity, 0.5f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Power curve applied to AO.\nHigher = darker/stronger occlusion effect.");
			if (radius_changed || intensity_changed)
				m_event_bus.emitImmediate(GtaoParametersChangedEvent{ctx.settings.gtao_radius, ctx.settings.gtao_intensity});
			ImGui::Unindent();
		}
	}

	// --- Reflections ---
	if (ImGui::CollapsingHeader("Reflections")) {
		ImGui::Checkbox("SSR", &ctx.settings.ssr_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Traces the depth buffer and reflects last frame's image.\nRequires geometry pre-pass and IBL enabled.");
		if (ctx.settings.ssr_enabled) {
			ImGui::Indent();
			ImGui::Checkbox("Half Resolution##ssr", &ctx.settings.ssr_half_res);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("4x fewer rays, bilinear upsampled.");
			bool steps_changed = ImGui::SliderInt("Max Steps", &ctx.settings.ssr_max_steps, 8, 128);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Hi-Z traversal iteration cap per ray.");
			bool thickness_changed = ImGui::SliderFloat("Thickness", &ctx.settings.ssr_thickness, 0.05f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("View-space depth tolerance for hits.");
			bool dist_changed = ImGui::SliderFloat("Max Distance", &ctx.settings.ssr_max_distance, 5.0f, 100.0f, "%.0f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("World-space ray length.");
			if (steps_changed || thickness_changed || dist_changed)
				m_event_bus.emitImmediate(SsrParametersChangedEvent{ctx.settings.ssr_max_steps,
					ctx.settings.ssr_thickness, ctx.settings.ssr_max_roughness, ctx.settings.ssr_max_distance});
			ImGui::Unindent();
		}
		ImGui::SliderFloat("Max Roughness", &ctx.settings.ssr_max_roughness, 0.0f, 1.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("SSR gives up above this roughness.");
	}

	// --- Global Illumination ---
	if (ImGui::CollapsingHeader("Global Illumination")) {
		ImGui::Checkbox("Radiance Cascades", &ctx.settings.rc.enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Indirect diffuse light: surfaces pick up bounced colour from\ntheir surroundings. Needs the geometry pre-pass.");
		if (ctx.settings.rc.enabled) {
			ImGui::Indent();

			ImGui::SeparatorText("Look");
			ImGui::SliderFloat("GI Intensity", &ctx.settings.rc.intensity, 0.0f, 2.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Scales the bounce light the probes collect. The sky share is not scaled.");
			ImGui::SliderFloat("Spec Occlusion", &ctx.settings.rc.spec_occlusion, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Scales the environment reflection by how much sky a surface can\nactually see.\n\nDarkens only; it never brightens.");
			ImGui::Checkbox("Rough Specular", &ctx.settings.rc.rough_specular);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rough surfaces reflect the probes' measured scene light\ninstead of the environment map. Mirror-like surfaces keep\nSSR and the environment map.");
			if (ctx.settings.rc.rough_specular) {
				ImGui::SliderFloat("Spec Handoff", &ctx.settings.rc.spec_handoff_roughness, 0.0f, 1.0f, "%.2f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Roughness at which the probes fully replace the environment map.\nBelow it the probes fade out.");
			}
			ImGui::SliderFloat("Short Range AO", &ctx.settings.rc.short_range_ao, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Applies GTAO to RC's share of diffuse ambient.");
			ImGui::SliderFloat("Bounce Luma Clamp", &ctx.settings.rc.luma_clamp, 1.0f, 200.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Caps how bright a single bounce can be.");

			ImGui::SliderFloat("Sky Proof Fraction",
				&ctx.settings.rc.sky_min_frac, 0.0f, 1.0f, "%.4f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Fraction of a ray's full range that must be seen empty before\nleaving the screen through sky counts as an open sky direction.");
			ImGui::SliderFloat("Sky Visibility", &ctx.settings.rc.sky_visibility, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("How much the probes' estimate of the sky each direction sees\nreplaces the environment map (or flat ambient).\n0: the environment lights every surface unoccluded, the probes\nadd bounce only. 1: the probes' estimate lights the diffuse sky\nand occludes the environment reflection. Between: a linear blend.");
			ImGui::Checkbox("History Depth Check",
				&ctx.settings.rc.hist_depth_check);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Only take bounce colour from last frame where the surface was\nactually visible last frame.");

			ImGui::SeparatorText("Probe grid");
			ImGui::SliderFloat("Probe Spacing", &ctx.settings.rc.ds0, 0.01f, 2.0f, "%.3f m",
				ImGuiSliderFlags_Logarithmic);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Distance between c0 probes.");
			sliderAppliedOnRelease("Probe Directions", ctx.settings.rc.c0_polar_bins, m_rc_held.c0_polar_bins,
				[](const char* label, int* v) {
					std::string directions_label = std::format("{} per probe", 2 * *v * *v);
					ImGui::SliderInt(label, v, 2, 8, directions_label.c_str());
				});
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Directions each probe stores at the finest level; every level out\nstores four times as many.");
			sliderAppliedOnRelease("Ray Length", ctx.settings.rc.c0_ray_length, m_rc_held.c0_ray_length,
				[](const char* label, float* v) { ImGui::SliderFloat(label, v, 0.1f, 64.0f, "%.2f x spacing"); });
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("How far the finest level's rays reach, as a multiple of Probe\nSpacing; each level out reaches four times further.");
			ImGui::SliderFloat("LOD 0 Distance", &ctx.settings.rc.lod0_dist, 1.0f, 10.0f, "%.0f m");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("How far from the camera the finest probe spacing reaches.\nBeyond it the spacing doubles at each step out.");
			ImGui::SliderFloat("LOD Blend Zone", &ctx.settings.rc.blend_zone, 0.0f, 0.5f, "%.2f");
			ImGui::SliderInt("Probe TTL", &ctx.settings.rc.probe_ttl, 0, 8000);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Frames a probe is kept after it stops being visible.");

			ImGui::SeparatorText("Quality and cost");
			ImGui::SliderFloat("Surface Thickness", &ctx.settings.rc.thickness, 0.05f, 20.0f, "%.2f m");
			ImGui::SliderInt("Rays / Pixel", &ctx.settings.rc.rays_per_pixel, 1, 16);

			const char* res_labels[] = {"Full", "Half", "Quarter", "Eighth"};
			const int res_values[] = {1, 2, 4, 8};
			int res_idx = ctx.settings.rc.resolution_div >= 8 ? 3
				: (ctx.settings.rc.resolution_div >= 4 ? 2
				: (ctx.settings.rc.resolution_div >= 2 ? 1 : 0));
			if (ImGui::Combo("GI Resolution", &res_idx, res_labels, IM_ARRAYSIZE(res_labels)))
				ctx.settings.rc.resolution_div = res_values[res_idx];
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Resolution the GI is computed at.\nFewer pixels means fewer probes get discovered.");
			sliderAppliedOnRelease("Cascades", ctx.settings.rc.cascades, m_rc_held.cascades,
				[](const char* label, int* v) { ImGui::SliderInt(label, v, 1, 6); });
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Each level out reaches four times further.\nBeyond the last level, distant bounce is replaced by a plain sky\nestimate.");
			sliderAppliedOnRelease("Store Budget", ctx.settings.rc.memory_mb, m_rc_held.memory_mb,
				[](const char* label, int* v) { ImGui::SliderInt(label, v, 32, 4096, "%d MB"); });
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Memory for per-direction probe data");

			const char* deposit_labels[] = {"Nearest probe", "All 8 weighted", "Random corner"};
			int deposit_mode = std::clamp(ctx.settings.rc.deposit_mode, 0, 2);
			if (ImGui::Combo("Deposit", &deposit_mode, deposit_labels, IM_ARRAYSIZE(deposit_labels)))
				ctx.settings.rc.deposit_mode = deposit_mode;
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Which probes a pixel registers and its rays inform.\nNearest probe: the probe nearest the pixel, then that probe's nearest\nparent at each level out, each with the ray's full share.\nAll 8 weighted: the 8 probes around the pixel, by trilinear share.\nRandom corner: one of those 8, drawn once per pixel each frame with those\nshares as probabilities.");
			ImGui::Checkbox("Count-Weighted Read",
				&ctx.settings.rc.count_weighted_read);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("When a probe reads its parents, each parent is weighted by its slot's\nsample count (frames with samples, up to 63) on top of its position.\nOff: position only. Parents with no samples are skipped either way.");
			const char* parent_labels[] = {"All 8", "Nearest"};
			int parent_mode = std::clamp(ctx.settings.rc.parent_mode, 0, 1);
			if (ImGui::Combo("Parent Insertion", &parent_mode, parent_labels, IM_ARRAYSIZE(parent_labels)))
				ctx.settings.rc.parent_mode = parent_mode;
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Which probes of the next level each probe registers: all 8 around it,\nor the single nearest one.");

			ImGui::SeparatorText("Diagnostics");
			ImGui::Checkbox("Freeze Probe Insertion", &ctx.settings.rc.freeze_probes);
			ImGui::Checkbox("Single Frame", &ctx.settings.rc.single_frame);
			if (ImGui::Button("Reset Store"))
				ctx.settings.rc.reset_requests++;

			ImGui::Unindent();
		}
	}

	// --- LOD ---
	if (ImGui::CollapsingHeader("Level of Detail")) {
		const char* lod_items[] = {"Auto", "LOD 0", "LOD 1", "LOD 2", "LOD 3"};
		int lod_combo = ctx.settings.lod_force_level + 1;
		if (ImGui::Combo("Force LOD", &lod_combo, lod_items, 5))
			ctx.settings.lod_force_level = lod_combo - 1;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("CPU backend only.\nAuto = screen-size-based selection.");
		if (ctx.settings.lod_force_level < 0) {
			ImGui::SliderFloat("LOD 0->1", &ctx.settings.lod_screen_thresholds[0], 0.01f, 1.0f, "%.3f");
			ImGui::SliderFloat("LOD 1->2", &ctx.settings.lod_screen_thresholds[1], 0.01f, 0.5f, "%.3f");
			ImGui::SliderFloat("LOD 2->3", &ctx.settings.lod_screen_thresholds[2], 0.001f, 0.2f, "%.3f");
			ImGui::SliderFloat("Hysteresis", &ctx.settings.lod_hysteresis, 0.0f, 0.5f, "%.2f");
		}
	}

	// --- Post Processing ---
	if (ImGui::CollapsingHeader("Post Processing")) {
		ImGui::SliderInt("Blur Radius", &ctx.settings.blur_radius, 0, 10);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Gaussian blur kernel radius.\n0 = No blur");
		ImGui::SliderFloat("Blur Strength", &ctx.settings.blur_strength, 0.0f, 5.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Spacing between blur samples, in texels.");
		ImGui::SliderFloat("Exposure", &ctx.settings.exposure, 0.0f, 5.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Tone mapping exposure adjustment.\n< 1.0: Darker, > 1.0: Brighter");
		{
			static const char* tone_map_names_sdr[] = {"None", "Reinhard", "ACES Fitted", "PBR Neutral", "GT Tonemap"};
			static const char* tone_map_names_hdr[] = {"None", "GT Tonemap"};
			static const char* tone_map_descriptions[] = {
				"No tone mapping.",
				"Per-channel highlight compression: color/(color+1).",
				"ACES fitted (Stephen Hill). sRGB -> AP1 color space,\nRRT+ODT fit.",
				"Khronos PBR Neutral.",
				"Gran Turismo tonemap (Uchimura). Adjustable peak\nbrightness for HDR. Toe + linear + shoulder curve.",
			};
			if (ctx.settings.hdr_enabled) {
				int hdr_idx = (ctx.settings.tone_map_mode == TONEMAP_GT) ? 1 : 0;
				ImGui::Combo("Tone Mapping (HDR)", &hdr_idx, tone_map_names_hdr, 2);
				ctx.settings.tone_map_mode = (hdr_idx == 1) ? TONEMAP_GT : TONEMAP_NONE;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", tone_map_descriptions[ctx.settings.tone_map_mode]);
				ImGui::TextDisabled("HDR output: only None and GT apply");
			} else {
				ImGui::Combo("Tone Mapping (SDR)", &ctx.settings.tone_map_mode, tone_map_names_sdr, 5);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", tone_map_descriptions[ctx.settings.tone_map_mode]);
			}
			if (ctx.settings.hdr_enabled && ctx.settings.tone_map_mode == TONEMAP_GT) {
				ImGui::SliderFloat("Peak White", &ctx.settings.hdr_peak_white, 1.0f, 20.0f, "%.1f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Peak output of the GT curve.\nHDR10: 1.0 = 200 nits.");
			}
		}

		ImGui::Separator();
		ImGui::Checkbox("Bloom Enabled", &ctx.settings.bloom_enabled);
		ImGui::DragFloat("Bloom Strength", &ctx.settings.bloom_strength, 0.001f, 0.0f, 0.4f);
	}

	// --- Particles ---
	if (m_particles && ImGui::CollapsingHeader("Particles", ImGuiTreeNodeFlags_DefaultOpen)) {
		uint32_t capacity = m_particles->getCapacity();
		uint32_t pending = m_particles->getPendingCapacity();
		ImGui::Text("Allocated: %u", capacity);

		uint32_t ceiling = m_max_particle_capacity > 0 ? m_max_particle_capacity : 1;
		int pending_int = static_cast<int>(pending);
		if (ImGui::DragInt("Capacity", &pending_int, 100.0f, 1, static_cast<int>(ceiling))) {
			pending_int = std::clamp(pending_int, 1, static_cast<int>(ceiling));
			m_particles->stageCapacity(static_cast<uint32_t>(pending_int));
		}

		bool has_pending = m_particles->hasPendingCapacity();
		if (!has_pending)
			ImGui::BeginDisabled();
		if (ImGui::Button("Apply"))
			m_particles->applyStagedCapacity();
		if (!has_pending)
			ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("(reallocates GPU buffers)");
	}

	// --- Physics ---
	if (ImGui::CollapsingHeader("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Physics Simulation", &ctx.sim.physics_enabled);
	}

	ImGui::End();
}

} // namespace ve
