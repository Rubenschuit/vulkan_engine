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
#include <algorithm>
#include <string>
#include <vector>

namespace ve {

using namespace ve::ui;

void GraphicsPanel::render(Registry* /*registry*/, EditorState& state, UIContext& ctx) {
	if (!ImGui::Begin("Graphics", &state.show_settings, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	// --- Display ---
	if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("FOV (deg)", &ctx.settings.fov, 30.0f, 120.0f, "%.1f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Field of View in degrees.");

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
			ImGui::SetTooltip("Synchronizes frame rate with display refresh rate.\nEliminates screen tearing but may increase input latency.");

		bool hdr_supported = m_renderer.hasHdrSupport();
		if (!hdr_supported)
			ImGui::BeginDisabled();
		if (ImGui::Checkbox("Enable HDR", &ctx.settings.hdr_enabled))
			m_renderer.setHdrEnabled(ctx.settings.hdr_enabled);
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
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("High Dynamic Range.\nRequires compatible HDR display.");

		ImGui::Separator();
		auto extent = m_renderer.getExtent();
		ImGui::Text("Resolution: %d x %d", extent.width, extent.height);
	}

	// --- Culling ---
	if (ImGui::CollapsingHeader("Culling", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Checkbox("Depth Pre-pass", &ctx.settings.depth_prepass_enabled))
			m_event_bus.emitImmediate(DepthPrePassChangedEvent{ctx.settings.depth_prepass_enabled});
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Early depth pass. Required by GTAO, shadow mask, SSR, and Hi-Z occlusion culling.");
		ImGui::Checkbox("Frustum Culling", &ctx.settings.enable_frustum_culling);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Skip drawing objects outside the camera view");
		ImGui::Text("Culling Backend");
		int backend = static_cast<int>(ctx.settings.culling_backend);
		ImGui::RadioButton("CPU", &backend, static_cast<int>(CullingBackendMode::CPU));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("CPU frustum culling");
		ImGui::RadioButton("GPU", &backend, static_cast<int>(CullingBackendMode::GPU));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("GPU-driven frustum culling via compute shader.\nImproves performance for large scenes.");
		ImGui::RadioButton("Meshlet", &backend, static_cast<int>(CullingBackendMode::MESHLET));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Sub-object meshlet culling (frustum + backface cone + Hi-Z).\nReduces triangle count for partially-visible large meshes.\nMore overhead on Apple silicon.");
		ctx.settings.culling_backend = static_cast<CullingBackendMode>(backend);

		ImGui::BeginDisabled(ctx.settings.culling_backend == CullingBackendMode::CPU);
		ImGui::Checkbox("Hi-Z Occlusion Culling", &ctx.settings.hiz_occlusion_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Skips objects occluded by the depth buffer.\nRequires depth pre-pass and a GPU/Meshlet backend.");
		ImGui::EndDisabled();

		if (ctx.settings.culling_backend == CullingBackendMode::MESHLET) {
			ImGui::Indent();
			ImGui::Checkbox("Object Culled Shadows", &ctx.settings.meshlet_object_culled_shadows);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Also spin up the GPU culling system and use its object-level shadow path.\nFaster shadow culling in some cases (especially on MoltenVK),\nat the cost of extra VRAM.");
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
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::Text("Multi-Sample Anti-Aliasing");
			ImGui::Separator();
			ImGui::TextColored(COL_MUTED, "Smooths jagged edges on geometry.");
			ImGui::Text("Off (1x): No MSAA, best performance");
			ImGui::Text("2x-4x: Balanced quality/performance");
			ImGui::Text("8x+: High quality, significant cost");
			ImGui::EndTooltip();
		}
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
			ImGui::Text("Shadow Rendering Mode");
			ImGui::Separator();
			ImGui::Text("Off: No shadows, best performance");
			ImGui::Text("Normal: Hard shadows, good performance");
			ImGui::Text("PCF: Percentage Closer Filtering");
			ImGui::TextColored(COL_MUTED, "     Soft shadow edges, higher cost");
			ImGui::Text("PCSS: Percentage Closer Soft Shadows");
			ImGui::TextColored(COL_MUTED, "     Contact-hardening soft shadows, highest cost");
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
				ImGui::SetTooltip("Poisson disk samples for PCSS variable-radius filter.\nHigher = smoother soft shadows.");
		}
		if (ctx.settings.shadow_mode != ShadowMode::DISABLED) {
			ImGui::SliderFloat("Shadow Bias", &ctx.settings.shadow_bias, 0.0f, 0.01f, "%.5f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Depth comparison bias.\nHigher = less acne but more Peter panning.");
			ImGui::SliderFloat("Normal Bias", &ctx.settings.csm_normal_bias, 0.0f, 8.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("CSM normal offset in shadow texels (auto-scaled per cascade and by\nangle to the light). Pushes the shadow lookup along the surface normal.\nHigher = less acne on angled surfaces, slightly more Peter panning.");
			ImGui::SliderFloat("Depth Bias Constant", &ctx.settings.depth_bias_constant, 0.0f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rasterizer constant depth bias applied when rendering shadow maps.\nHigher = less acne but more Peter panning.");
			ImGui::SliderFloat("Depth Bias Slope", &ctx.settings.depth_bias_slope, 0.0f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rasterizer slope-scaled depth bias applied when rendering shadow maps.\nScales with surface slope relative to light direction.");
			ImGui::SliderFloat("Depth Bias Clamp", &ctx.settings.depth_bias_clamp, 0.0f, 0.1f, "%.4f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Maximum absolute depth bias. Caps the total bias to prevent\nPeter panning on surfaces close to the light.");
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
				ImGui::SetTooltip("Screen-space shadow mask: evaluates CSM shadows\nonce per pixel via async compute (1-frame latency).\nRequires depth pre-pass enabled.");
			if (ctx.settings.shadow_mask_enabled) {
				ImGui::Indent();
				ImGui::Checkbox("Half Resolution", &ctx.settings.shadow_mask_half_res);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Evaluate shadow mask at half screen resolution.\n4x fewer compute invocations, bilinear upsampled.");
				ImGui::Unindent();
			}
		}
	}

	// --- Ambient Occlusion ---
	if (ImGui::CollapsingHeader("Ambient Occlusion")) {
		ImGui::Checkbox("GTAO", &ctx.settings.gtao_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Ground Truth Ambient Occlusion:\ndarkens corners, crevices, and contact areas.\nRequires depth pre-pass enabled.");
		if (ctx.settings.gtao_enabled) {
			ImGui::Indent();
			ImGui::Checkbox("Half Resolution##gtao", &ctx.settings.gtao_half_res);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Compute AO at half screen resolution.\n4x fewer compute invocations, bilinear upsampled.");
			bool radius_changed = ImGui::SliderFloat("AO Radius", &ctx.settings.gtao_radius, 0.1f, 3.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("World-space sampling radius for AO.\nLarger = broader occlusion, smaller = finer detail.");
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
			ImGui::SetTooltip("Screen-space reflections: traces the depth buffer\nand reflects last frame's image; falls back to IBL.\nRequires depth pre-pass enabled.");
		if (ctx.settings.ssr_enabled) {
			ImGui::Indent();
			ImGui::Checkbox("Half Resolution##ssr", &ctx.settings.ssr_half_res);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Trace at half screen resolution.\n4x fewer rays, bilinear upsampled.");
			bool steps_changed = ImGui::SliderInt("Max Steps", &ctx.settings.ssr_max_steps, 8, 64);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Ray-march samples per pixel.");
			bool thickness_changed = ImGui::SliderFloat("Thickness", &ctx.settings.ssr_thickness, 0.05f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("View-space depth tolerance for hits.");
			bool rough_changed = ImGui::SliderFloat("Max Roughness", &ctx.settings.ssr_max_roughness, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Surfaces rougher than this fall back to IBL.");
			bool dist_changed = ImGui::SliderFloat("Max Distance", &ctx.settings.ssr_max_distance, 5.0f, 100.0f, "%.0f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("World-space ray length.");
			if (steps_changed || thickness_changed || rough_changed || dist_changed)
				m_event_bus.emitImmediate(SsrParametersChangedEvent{ctx.settings.ssr_max_steps,
					ctx.settings.ssr_thickness, ctx.settings.ssr_max_roughness, ctx.settings.ssr_max_distance});
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
			ImGui::SetTooltip("Force all meshes to a specific LOD level.\nAuto = normal distance-based selection.");
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
			ImGui::SetTooltip("Gaussian blur kernel size.\n0 = No blur, higher values = stronger blur effect");
		ImGui::SliderFloat("Blur Strength", &ctx.settings.blur_strength, 0.0f, 5.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Intensity of the blur effect");
		ImGui::SliderFloat("Exposure", &ctx.settings.exposure, 0.0f, 5.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Tone mapping exposure adjustment.\n< 1.0: Darker, > 1.0: Brighter");
		{
			static const char* tone_map_names_sdr[] = {"None", "Reinhard", "ACES Fitted", "PBR Neutral", "GT Tonemap"};
			static const char* tone_map_names_hdr[] = {"None", "GT Tonemap"};
			static const char* tone_map_descriptions[] = {
				"No tone mapping. Raw linear values clamped by display.",
				"Simple highlight compression: color/(color+1).\nPreserves hue, gentle rolloff.",
				"ACES fitted (Stephen Hill). sRGB -> AP1 color space,\nRRT+ODT fit. Rich midtones, cinematic look.",
				"Khronos PBR Neutral. Preserves material base\ncolors under grayscale lighting.",
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
					ImGui::SetTooltip("GT tonemap peak brightness in scene-linear units.\n4.0 ~ 320 nits, 10.0 ~ 800 nits");
			}
		}

		ImGui::Separator();
		ImGui::Checkbox("Bloom Enabled", &ctx.settings.bloom_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Bloom effect creates glow around bright areas.\nSimulates light bleeding in cameras");
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
