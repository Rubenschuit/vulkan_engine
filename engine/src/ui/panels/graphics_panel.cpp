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
	// Negative: widgets stop this far short of the right edge, so labels keep their room under Indent()
	ImGui::PushItemWidth(-ImGui::GetFontSize() * 11.0f);

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
					std::string preview = (current.width > 0) ? label(current) : "Desktop Default";
					if (ImGui::BeginCombo("Video Mode", preview.c_str())) {
						bool desktop_selected = (current.width == 0);
						if (ImGui::Selectable("Desktop Default", desktop_selected))
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
		}

		if (ImGui::Checkbox("VSync", &ctx.settings.vsync))
			m_renderer.setVSync(ctx.settings.vsync);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Present mode: FIFO when on, IMMEDIATE when off\n(FIFO if IMMEDIATE is unavailable).");

		bool hdr_supported = m_renderer.hasHdrSupport();
		if (!hdr_supported)
			ImGui::BeginDisabled();
		if (ImGui::Checkbox("HDR", &ctx.settings.hdr_enabled))
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
		if (ImGui::Checkbox("Geometry Pre-Pass", &ctx.settings.geometry_prepass_enabled))
			m_event_bus.emitImmediate(GeometryPrePassChangedEvent{ctx.settings.geometry_prepass_enabled});
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Early depth + normal/roughness G-buffer pass.\nRequired by GTAO, shadow mask, SSR, Radiance Cascades, and Hi-Z occlusion culling.");
		ImGui::Checkbox("Frustum Culling", &ctx.settings.enable_frustum_culling);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("CPU backend only; GPU and Meshlet always frustum cull.");
		ImGui::Text("Backend");
		ImGui::SameLine();
		int backend = static_cast<int>(ctx.settings.culling_backend);
		ImGui::RadioButton("CPU", &backend, static_cast<int>(CullingBackendMode::CPU));
		ImGui::SameLine();
		ImGui::RadioButton("GPU", &backend, static_cast<int>(CullingBackendMode::GPU));
		ImGui::SameLine();
		ImGui::RadioButton("Meshlet", &backend, static_cast<int>(CullingBackendMode::MESHLET));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Sub-object meshlet culling (frustum + backface cone + Hi-Z).");
		ctx.settings.culling_backend = static_cast<CullingBackendMode>(backend);

		ImGui::BeginDisabled(ctx.settings.culling_backend == CullingBackendMode::CPU);
		ImGui::Checkbox("Hi-Z Occlusion Culling", &ctx.settings.hiz_occlusion_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Skips objects occluded by the depth buffer.\nNeeds the geometry pre-pass and a GPU/Meshlet backend.");
		ImGui::EndDisabled();

		if (ctx.settings.culling_backend == CullingBackendMode::MESHLET) {
			ImGui::Indent();
			ImGui::Checkbox("Object Culled Shadows", &ctx.settings.meshlet_object_culled_shadows);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Also spin up the GPU culling system and use its object-level shadow path.\nCosts extra VRAM.");
			ImGui::Unindent();
		}
		ImGui::SeparatorText("Multi-Threading");
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

		if (ImGui::BeginCombo("MSAA", sample_labels[current_index].c_str())) {
			for (size_t i = 0; i < available.size(); i++) {
				bool is_selected = (i == current_index);
				if (ImGui::Selectable(sample_labels[i].c_str(), is_selected) && !is_selected)
					m_renderer.setSampleCountInt(available[i]);
				if (is_selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}

	// --- Shadows ---
	if (ImGui::CollapsingHeader("Shadows")) {
		int shadow_mode_int = static_cast<int>(ctx.settings.shadow_mode);
		if (ImGui::Combo("Mode", &shadow_mode_int, "Off\0" "Normal\0" "PCF\0" "PCSS\0"))
			ctx.settings.shadow_mode = static_cast<ShadowMode>(shadow_mode_int);
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
			ImGui::SliderFloat("Shadow Distance", &ctx.settings.csm_distance, 10.0f, 1000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("How far from the camera the sun's cascades reach. Shorter spreads the same\n"
					"shadow texels over less of the scene: sharper up close. Past it, surfaces are unshadowed.");
		}
		if (ctx.settings.shadow_mode == ShadowMode::PCSS) {
			ImGui::SliderFloat("Light Size", &ctx.settings.pcss_light_size, 0.001f, 0.2f, "%.3f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Virtual light size for spot/point PCSS penumbra.\nLarger = softer shadows farther from caster.");
		}
		if (ctx.settings.shadow_mode == ShadowMode::PCF || ctx.settings.shadow_mode == ShadowMode::PCSS) {
			static constexpr int pcf_values[] = {4, 8, 16, 32};
			int pcf_idx = 0;
			for (int j = 0; j < 4; j++)
				if (ctx.settings.pcf_samples == pcf_values[j])
					pcf_idx = j;
			bool pcf = ctx.settings.shadow_mode == ShadowMode::PCF;
			const char* items = pcf ? "3x3 Tent\0" "5x5 Tent\0" "7x7 Tent\0" "9x9 Tent\0" : "4\0" "8\0" "16\0" "32\0";
			if (ImGui::Combo(pcf ? "PCF Filter" : "Blocker Samples", &pcf_idx, items))
				ctx.settings.pcf_samples = pcf_values[pcf_idx];
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(pcf ? "Tent filter size in shadow texels (4/9/16/25 bilinear taps)."
				                      : "PCSS blocker search taps, each gathering 2x2 texels.");
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
			ImGui::Text("Shadow Cull");
			ImGui::SameLine();
			ImGui::RadioButton("Front##scull", &cull_idx, 0);
			ImGui::SameLine();
			ImGui::RadioButton("Back##scull", &cull_idx, 1);
			ImGui::SameLine();
			ImGui::RadioButton("None##scull", &cull_idx, 2);
			ctx.settings.shadow_cull_mode = static_cast<ShadowCullMode>(cull_idx);
			ImGui::Text("CSM Blend");
			ImGui::SameLine();
			ImGui::RadioButton("Off", &ctx.settings.csm_blend_mode, 0);
			ImGui::SameLine();
			ImGui::RadioButton("Linear", &ctx.settings.csm_blend_mode, 1);
			ImGui::SameLine();
			ImGui::RadioButton("Dithered", &ctx.settings.csm_blend_mode, 2);
			ImGui::Checkbox("Shadow Mask (Async Compute)", &ctx.settings.shadow_mask_enabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Evaluates CSM shadows once per pixel.\nNeeds the geometry pre-pass.");
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
			ImGui::SetTooltip("Needs the geometry pre-pass.");
		if (ctx.settings.gtao_enabled) {
			ImGui::Indent();
			ImGui::Checkbox("Half Resolution##gtao", &ctx.settings.gtao_half_res);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("4x fewer compute invocations, bilinear upsampled.");
			bool radius_changed = ImGui::SliderFloat("AO Radius", &ctx.settings.gtao_radius, 0.01f, 3.0f, "%.3f m",
				ImGuiSliderFlags_Logarithmic);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("World-space sampling radius.\nOccluders fade out by twice this distance.");
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
			ImGui::SetTooltip("Traces the depth buffer and reflects last frame's image.\nNeeds the geometry pre-pass.");
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
			ImGui::SliderFloat("Max Roughness", &ctx.settings.ssr_max_roughness, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("SSR gives up above this roughness.");
			ImGui::Unindent();
		}
	}

	// --- Global Illumination ---
	if (ImGui::CollapsingHeader("Global Illumination")) {
		ImGui::Checkbox("Radiance Cascades", &ctx.settings.rc.enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Indirect diffuse light: surfaces pick up bounced colour from\ntheir surroundings. Needs the geometry pre-pass.");
		if (ctx.settings.rc.enabled) {
			ImGui::Indent();

			bool ray_query = m_ray_query_supported && ctx.settings.rc.ray_query;
			const char* sky_labels[] = {ray_query ? "Auto (Estimate)" : "Auto (Off)", "Off", "Unknown = Sky",
				"Probe Estimate", "Unknown = Blocked"};
			int sky_mode = static_cast<int>(ctx.settings.rc.sky_occlusion);
			if (ImGui::Combo("Sky Occlusion", &sky_mode, sky_labels, IM_ARRAYSIZE(sky_labels)))
				ctx.settings.rc.sky_occlusion = static_cast<RcSkyOcclusion>(sky_mode);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("How the probes' view of the sky darkens the environment (IBL, or the flat ambient)."
					"\nOff: the environment as with GI off; GI adds bounced light only."
					"\nProbe Estimate: directions no ray resolved take the probe's proven-sky share."
					"\nUnknown = Sky / Blocked: directions no ray resolved count as open sky / as blocked."
					"\nThe screen-space tracer leaves many rays unresolved, so Auto turns it off there.");
			ImGui::Checkbox("Rough Specular", &ctx.settings.rc.specular);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rough surfaces (roughness 0.3 and up) reflect their surroundings, not just the sky."
					"\nOff: they reflect only the sky."
					"\nSmoother surfaces are not affected.");

			ImGui::SeparatorText("Tracing");
			ImGui::BeginDisabled(!m_ray_query_supported);
			if (ImGui::Checkbox("Ray Query", &ray_query))
				ctx.settings.rc.ray_query = ray_query;
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Trace the scene's acceleration structures, which also see geometry off screen."
					"\nOff: march the depth pyramid, which sees only what is on screen, and free the structures.");
			ImGui::EndDisabled();
			if (!m_ray_query_supported) {
				ImGui::SameLine();
				ImGui::TextDisabled("(Not supported by device)");
			}
			ImGui::Indent();
			ImGui::BeginDisabled(!ray_query);
			ImGui::Checkbox("Deformed Meshes", &ctx.settings.rc.trace_deformed);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Put skinned and morphed meshes in the acceleration structures,"
					"\nrefitted to their pose every frame."
					"\nOff: rays pass through them.");
			ImGui::EndDisabled();
			ImGui::Unindent();

			const char* res_labels[] = {"Full", "Half", "Quarter", "Eighth"};
			const int res_values[] = {1, 2, 4, 8};
			int res_idx = ctx.settings.rc.resolution_div >= 8 ? 3
				: (ctx.settings.rc.resolution_div >= 4 ? 2
				: (ctx.settings.rc.resolution_div >= 2 ? 1 : 0));
			if (ImGui::Combo("Resolution", &res_idx, res_labels, IM_ARRAYSIZE(res_labels)))
				ctx.settings.rc.resolution_div = res_values[res_idx];
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Resolution the GI is computed at.\nFewer pixels means fewer probes get discovered.");
			ImGui::SliderInt("Rays / Pixel", &ctx.settings.rc.rays_per_pixel, 1, 16);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rays each GI pixel traces per frame.");

			ImGui::SeparatorText("Probes");
			ImGui::SliderFloat("Spacing", &ctx.settings.rc.ds0, 0.01f, 2.0f, "%.3f m",
				ImGuiSliderFlags_Logarithmic);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Distance between c0 probes.");
			ImGui::SliderFloat("LOD 0 Range", &ctx.settings.rc.lod0_dist, 1.0f, 10.0f, "%.0f m");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("How far from the camera the finest probe spacing reaches.\nBeyond it the spacing doubles at each step out.");
			sliderAppliedOnRelease("Cascades", ctx.settings.rc.cascades, m_rc_held.cascades,
				[](const char* label, int* v) { ImGui::SliderInt(label, v, 1, 6); });
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Each level out reaches four times further.\nBeyond the last level, distant bounce is replaced by a plain sky\nestimate.");
			sliderAppliedOnRelease("Memory", ctx.settings.rc.memory_mb, m_rc_held.memory_mb,
				[](const char* label, int* v) { ImGui::SliderInt(label, v, 32, 4096, "%d MiB"); });
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Memory for the probes, which sets how many fit."
					"\nDebug > Radiance Cascades shows how full each cascade is.");
			ImGui::SliderInt("Lifetime", &ctx.settings.rc.probe_ttl, 0, 1024, "%d frames", ImGuiSliderFlags_Logarithmic);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Frames a probe is kept after no visible surface needs it."
					"\nLower keeps fewer probes alive, but an area you look away from and back at"
					"\nloses its lighting and takes a moment to build it up again.");

			ImGui::SeparatorText("Filtering");
			ImGui::SliderInt("History", &ctx.settings.rc.history_length, 4, RC_MAX_HISTORY_LENGTH, "%d frames");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Sampled frames each probe direction averages over."
					"\nLower follows moving lights and objects sooner, with more noise.");
			ImGui::SliderFloat("Luma Clamp", &ctx.settings.rc.luma_clamp, 1.0f, 200.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Caps how bright a single bounce can be.");

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
		ImGui::Checkbox("Bloom", &ctx.settings.bloom_enabled);
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

	ImGui::PopItemWidth();
	ImGui::End();
}

} // namespace ve
