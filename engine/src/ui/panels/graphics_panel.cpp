#include "pch.hpp"
#include "ui/panels/graphics_panel.hpp"
#include "ui/editor_state.hpp"
#include "ui/imgui_layer.hpp"
#include "rendering/ve_renderer.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>

namespace ve {

void GraphicsPanel::render(Registry* /*registry*/, EditorState& state, UIContext& ctx) {
	if (!ImGui::Begin("Graphics", &state.show_settings, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	// --- Anti-Aliasing ---
	if (ImGui::CollapsingHeader("Anti-Aliasing", ImGuiTreeNodeFlags_DefaultOpen)) {
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
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Smooths jagged edges on geometry.");
			ImGui::Text("Off (1x): No MSAA, best performance");
			ImGui::Text("2x-4x: Balanced quality/performance");
			ImGui::Text("8x+: High quality, significant cost");
			ImGui::EndTooltip();
		}
	}

	// --- Shadows ---
	if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::PushItemWidth(200.0f);
		int shadow_mode_int = static_cast<int>(ctx.shadow_mode);
		if (ImGui::SliderInt("##shadow_slider", &shadow_mode_int, 0, 3, ""))
			ctx.shadow_mode = static_cast<ShadowMode>(std::clamp(shadow_mode_int, 0, 3));
		ImGui::PopItemWidth();
		ImGui::SameLine();
		const char* shadow_labels[] = {"Off", "Normal", "PCF", "PCSS"};
		ImGui::Text("%s", shadow_labels[static_cast<uint32_t>(ctx.shadow_mode)]);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::Text("Shadow Rendering Mode");
			ImGui::Separator();
			ImGui::Text("Off: No shadows, best performance");
			ImGui::Text("Normal: Hard shadows, good performance");
			ImGui::Text("PCF: Percentage Closer Filtering");
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "     Soft shadow edges, higher cost");
			ImGui::Text("PCSS: Percentage Closer Soft Shadows");
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "     Contact-hardening soft shadows, highest cost");
			ImGui::EndTooltip();
		}
		if (ctx.shadow_mode == ShadowMode::PCSS) {
			ImGui::SliderFloat("Light Size", &ctx.pcss_light_size, 0.001f, 0.2f, "%.3f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Virtual light size for PCSS penumbra.\nLarger = softer shadows farther from caster.");
		}
		if (ctx.shadow_mode == ShadowMode::PCF || ctx.shadow_mode == ShadowMode::PCSS) {
			static constexpr int pcf_values[] = {4, 8, 16, 32};
			int pcf_idx = 0;
			for (int j = 0; j < 4; j++)
				if (ctx.pcf_samples == pcf_values[j])
					pcf_idx = j;
			if (ImGui::Combo("PCF Samples", &pcf_idx, "4\0" "8\0" "16\0" "32\0"))
				ctx.pcf_samples = pcf_values[pcf_idx];
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Poisson disk samples for PCF filtering.\nAlso used for PCSS blocker search.\nRequires pipeline recreation.");
		}
		if (ctx.shadow_mode == ShadowMode::PCSS) {
			static constexpr int pcss_values[] = {8, 16, 32};
			int pcss_idx = 0;
			for (int j = 0; j < 3; j++)
				if (ctx.pcss_filter_samples == pcss_values[j])
					pcss_idx = j;
			if (ImGui::Combo("PCSS Filter Samples", &pcss_idx, "8\0" "16\0" "32\0"))
				ctx.pcss_filter_samples = pcss_values[pcss_idx];
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Poisson disk samples for PCSS variable-radius filter.\nHigher = smoother soft shadows.\nRequires pipeline recreation.");
		}
		if (ctx.shadow_mode != ShadowMode::DISABLED) {
			ImGui::SliderFloat("Shadow Bias", &ctx.shadow_bias, 0.0f, 0.01f, "%.5f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Depth comparison bias.\nHigher = less acne but more Peter panning.");
			ImGui::SliderFloat("Normal Bias", &ctx.csm_normal_bias, 0.0f, 1.0f, "%.3f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("World-space normal offset for CSM shadows.\nPushes shadow lookup along surface normal.\nHigher = less acne on angled surfaces.");
			ImGui::SliderFloat("Depth Bias Constant", &ctx.depth_bias_constant, 0.0f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rasterizer constant depth bias applied when rendering shadow maps.\nHigher = less acne but more Peter panning.");
			ImGui::SliderFloat("Depth Bias Slope", &ctx.depth_bias_slope, 0.0f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rasterizer slope-scaled depth bias applied when rendering shadow maps.\nScales with surface slope relative to light direction.");
			ImGui::SliderFloat("Depth Bias Clamp", &ctx.depth_bias_clamp, 0.0f, 0.1f, "%.4f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Maximum absolute depth bias. Caps the total bias to prevent\nPeter panning on surfaces close to the light.");
			ImGui::Text("CSM Blend: ");
			ImGui::SameLine();
			ImGui::RadioButton("Off", &ctx.csm_blend_mode, 0);
			ImGui::SameLine();
			ImGui::RadioButton("Linear", &ctx.csm_blend_mode, 1);
			ImGui::SameLine();
			ImGui::RadioButton("Dithered", &ctx.csm_blend_mode, 2);
			ImGui::Checkbox("Shadow Mask (async compute)", &ctx.shadow_mask_enabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Screen-space shadow mask: evaluates CSM shadows\nonce per pixel via async compute (1-frame latency).\nRequires depth pre-pass enabled.");
			if (ctx.shadow_mask_enabled) {
				ImGui::Indent();
				ImGui::Checkbox("Half Resolution", &ctx.shadow_mask_half_res);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Evaluate shadow mask at half screen resolution.\n4x fewer compute invocations, bilinear upsampled.");
				ImGui::Unindent();
			}
		}
	}

	// --- Ambient Occlusion ---
	if (ImGui::CollapsingHeader("Ambient Occlusion", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("GTAO", &ctx.gtao_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Ground Truth Ambient Occlusion:\ndarkens corners, crevices, and contact areas.\nRequires depth pre-pass enabled.");
		if (ctx.gtao_enabled) {
			ImGui::Indent();
			ImGui::Checkbox("Half Resolution##gtao", &ctx.gtao_half_res);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Compute AO at half screen resolution.\n4x fewer compute invocations, bilinear upsampled.");
			bool radius_changed = ImGui::SliderFloat("AO Radius", &ctx.gtao_radius, 0.1f, 3.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("World-space sampling radius for AO.\nLarger = broader occlusion, smaller = finer detail.");
			bool intensity_changed = ImGui::SliderFloat("AO Intensity", &ctx.gtao_intensity, 0.5f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Power curve applied to AO.\nHigher = darker/stronger occlusion effect.");
			if (radius_changed || intensity_changed)
				m_event_bus.emitImmediate(GtaoParametersChangedEvent{ctx.gtao_radius, ctx.gtao_intensity});
			ImGui::Unindent();
		}
	}

	// --- Culling ---
	if (ImGui::CollapsingHeader("Culling", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Checkbox("Depth Pre-pass", &ctx.depth_prepass_enabled))
			m_event_bus.emitImmediate(DepthPrePassChangedEvent{ctx.depth_prepass_enabled});
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Early depth pass. Required by GTAO, shadow mask, and Hi-Z occlusion culling.");
		ImGui::Checkbox("Frustum culling", &ctx.enable_frustum_culling);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Skip drawing objects outside the camera view");
		ImGui::Checkbox("GPU Culling", &ctx.gpu_culling_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("GPU-driven frustum culling via compute shader.\n Improves performance for large scenes.");
		if (ctx.gpu_culling_enabled) {
			ImGui::Indent();
			ImGui::Checkbox("Hi-Z Occlusion Culling", &ctx.hiz_occlusion_enabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Skips objects occluded by depth buffer.\nRequires depth pre-pass enabled.");
			ImGui::Checkbox("Meshlet Culling", &ctx.meshlet_culling_enabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Sub-object meshlet culling (frustum + backface cone + Hi-Z).\nReduces triangle count for partially-visible large meshes.\n More overhead on Apple silicon.");
			if (ctx.meshlet_culling_enabled) {
				ImGui::Indent();
				if (ImGui::Checkbox("Object Culled Shadows", &ctx.meshlet_gpu_shadow_fallback))
					m_event_bus.emitImmediate(GpuShadowFallbackChangedEvent{ctx.meshlet_gpu_shadow_fallback});
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Use object-level GPU culling for shadows instead of meshlet culling.");
				ImGui::Unindent();
			}
			ImGui::Unindent();
		}
		if (ImGui::Checkbox("Clustered Lighting", &ctx.cluster_enabled))
			m_event_bus.emitImmediate(ClusterEnabledChangedEvent{ctx.cluster_enabled});
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Use 3D cluster grid to cull lights per-fragment");

		ImGui::Separator();
		ImGui::Text("Multi-threading:");
		ImGui::SliderInt("Min cull entities", &ctx.min_parallel_cull_entities, 0, 4096);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Minimum mesh entities to enable parallel cpu frustum culling.\n0 = always parallel");
	}

	// --- LOD ---
	if (ImGui::CollapsingHeader("Level of Detail")) {
		const char* lod_items[] = {"Auto", "LOD 0", "LOD 1", "LOD 2", "LOD 3"};
		int lod_combo = ctx.lod_force_level + 1;
		if (ImGui::Combo("Force LOD", &lod_combo, lod_items, 5))
			ctx.lod_force_level = lod_combo - 1;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Force all meshes to a specific LOD level.\nAuto = normal distance-based selection.");
		if (ctx.lod_force_level < 0) {
			ImGui::SliderFloat("LOD 0->1", &ctx.lod_screen_thresholds[0], 0.01f, 1.0f, "%.3f");
			ImGui::SliderFloat("LOD 1->2", &ctx.lod_screen_thresholds[1], 0.01f, 0.5f, "%.3f");
			ImGui::SliderFloat("LOD 2->3", &ctx.lod_screen_thresholds[2], 0.001f, 0.2f, "%.3f");
			ImGui::SliderFloat("Hysteresis", &ctx.lod_hysteresis, 0.0f, 0.5f, "%.2f");
		}
	}

	// --- Post Processing ---
	if (ImGui::CollapsingHeader("Post Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderInt("Blur Radius", &ctx.blur_radius, 0, 10);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Gaussian blur kernel size.\n0 = No blur, higher values = stronger blur effect");
		ImGui::SliderFloat("Blur Strength", &ctx.blur_strength, 0.0f, 5.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Intensity of the blur effect");
		ImGui::SliderFloat("Exposure", &ctx.exposure, 0.0f, 5.0f);
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
			if (ctx.hdr_enabled) {
				int hdr_idx = (ctx.tone_map_mode == TONEMAP_GT) ? 1 : 0;
				ImGui::Combo("Tone Mapping", &hdr_idx, tone_map_names_hdr, 2);
				ctx.tone_map_mode = (hdr_idx == 1) ? TONEMAP_GT : TONEMAP_NONE;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", tone_map_descriptions[ctx.tone_map_mode]);
			} else {
				ImGui::Combo("Tone Mapping", &ctx.tone_map_mode, tone_map_names_sdr, 5);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", tone_map_descriptions[ctx.tone_map_mode]);
			}
			if (ctx.hdr_enabled && ctx.tone_map_mode == TONEMAP_GT) {
				ImGui::SliderFloat("Peak White", &ctx.hdr_peak_white, 1.0f, 20.0f, "%.1f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("GT tonemap peak brightness in scene-linear units.\n4.0 ~ 320 nits, 10.0 ~ 800 nits");
			}
		}

		ImGui::Separator();
		ImGui::Checkbox("Bloom Enabled", &ctx.bloom_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Bloom effect creates glow around bright areas.\nSimulates light bleeding in cameras");
		ImGui::DragFloat("Bloom Strength", &ctx.bloom_strength, 0.001f, 0.0f, 0.4f);
	}

	// --- Display ---
	if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("FOV (Degrees)", &ctx.fov, 30.0f, 120.0f, "%.1f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Field of View in degrees.");

		if (ImGui::Checkbox("Enable VSync", &ctx.vsync))
			m_renderer.setVSync(ctx.vsync);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Synchronizes frame rate with display refresh rate.\nEliminates screen tearing but may increase input latency.");

		bool hdr_supported = m_renderer.hasHdrSupport();
		if (!hdr_supported)
			ImGui::BeginDisabled();
		if (ImGui::Checkbox("Enable HDR", &ctx.hdr_enabled))
			m_renderer.setHdrEnabled(ctx.hdr_enabled);
		if (hdr_supported && ctx.hdr_enabled) {
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

	// --- Physics ---
	if (ImGui::CollapsingHeader("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Physics Simulation", &ctx.physics_enabled);
	}

	ImGui::End();
}

} // namespace ve
