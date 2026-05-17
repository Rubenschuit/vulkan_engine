#include "pch.hpp"
#include "ui/panels/graphics_panel.hpp"
#include "ui/editor_state.hpp"
#include "ui/imgui_layer.hpp"
#include "rendering/particle_backend.hpp"
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
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "     Soft shadow edges, higher cost");
			ImGui::Text("PCSS: Percentage Closer Soft Shadows");
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "     Contact-hardening soft shadows, highest cost");
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
				ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
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
				ImGui::SetTooltip("Poisson disk samples for PCF filtering.\nAlso used for PCSS blocker search.\nRequires pipeline recreation.");
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
				ImGui::SetTooltip("Poisson disk samples for PCSS variable-radius filter.\nHigher = smoother soft shadows.\nRequires pipeline recreation.");
		}
		if (ctx.settings.shadow_mode != ShadowMode::DISABLED) {
			ImGui::SliderFloat("Shadow Bias", &ctx.settings.shadow_bias, 0.0f, 0.01f, "%.5f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Depth comparison bias.\nHigher = less acne but more Peter panning.");
			ImGui::SliderFloat("Normal Bias", &ctx.settings.csm_normal_bias, 0.0f, 1.0f, "%.3f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("World-space normal offset for CSM shadows.\nPushes shadow lookup along surface normal.\nHigher = less acne on angled surfaces.");
			ImGui::SliderFloat("Depth Bias Constant", &ctx.settings.depth_bias_constant, 0.0f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rasterizer constant depth bias applied when rendering shadow maps.\nHigher = less acne but more Peter panning.");
			ImGui::SliderFloat("Depth Bias Slope", &ctx.settings.depth_bias_slope, 0.0f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Rasterizer slope-scaled depth bias applied when rendering shadow maps.\nScales with surface slope relative to light direction.");
			ImGui::SliderFloat("Depth Bias Clamp", &ctx.settings.depth_bias_clamp, 0.0f, 0.1f, "%.4f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Maximum absolute depth bias. Caps the total bias to prevent\nPeter panning on surfaces close to the light.");
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
	if (ImGui::CollapsingHeader("Ambient Occlusion", ImGuiTreeNodeFlags_DefaultOpen)) {
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

	// --- Culling ---
	if (ImGui::CollapsingHeader("Culling", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Checkbox("Depth Pre-pass", &ctx.settings.depth_prepass_enabled))
			m_event_bus.emitImmediate(DepthPrePassChangedEvent{ctx.settings.depth_prepass_enabled});
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Early depth pass. Required by GTAO, shadow mask, and Hi-Z occlusion culling.");
		ImGui::Checkbox("Frustum culling", &ctx.settings.enable_frustum_culling);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Skip drawing objects outside the camera view");
		ImGui::Checkbox("GPU Culling", &ctx.settings.gpu_culling_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("GPU-driven frustum culling via compute shader.\n Improves performance for large scenes.");
		if (ctx.settings.gpu_culling_enabled) {
			ImGui::Indent();
			ImGui::Checkbox("Hi-Z Occlusion Culling", &ctx.settings.hiz_occlusion_enabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Skips objects occluded by depth buffer.\nRequires depth pre-pass enabled.");
			ImGui::Checkbox("Meshlet Culling", &ctx.settings.meshlet_culling_enabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Sub-object meshlet culling (frustum + backface cone + Hi-Z).\nReduces triangle count for partially-visible large meshes.\n More overhead on Apple silicon.");
			if (ctx.settings.meshlet_culling_enabled) {
				ImGui::Indent();
				if (ImGui::Checkbox("Object Culled Shadows", &ctx.settings.meshlet_gpu_shadow_fallback))
					m_event_bus.emitImmediate(GpuShadowFallbackChangedEvent{ctx.settings.meshlet_gpu_shadow_fallback});
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Use object-level GPU culling for shadows instead of meshlet culling.");
				ImGui::Unindent();
			}
			ImGui::Unindent();
		}
		if (ImGui::Checkbox("Clustered Lighting", &ctx.settings.cluster_enabled))
			m_event_bus.emitImmediate(ClusterEnabledChangedEvent{ctx.settings.cluster_enabled});
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Use 3D cluster grid to cull lights per-fragment");

		ImGui::Separator();
		ImGui::Text("Multi-threading:");
		ImGui::SliderInt("Min cull entities", &ctx.settings.min_parallel_cull_entities, 0, 4096);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Minimum mesh entities to enable parallel cpu frustum culling.\n0 = always parallel");
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
	if (ImGui::CollapsingHeader("Post Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
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
				ImGui::Combo("Tone Mapping", &hdr_idx, tone_map_names_hdr, 2);
				ctx.settings.tone_map_mode = (hdr_idx == 1) ? TONEMAP_GT : TONEMAP_NONE;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", tone_map_descriptions[ctx.settings.tone_map_mode]);
			} else {
				ImGui::Combo("Tone Mapping", &ctx.settings.tone_map_mode, tone_map_names_sdr, 5);
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

	// --- Display ---
	if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("FOV (Degrees)", &ctx.settings.fov, 30.0f, 120.0f, "%.1f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Field of View in degrees.");

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

	// --- Physics ---
	if (ImGui::CollapsingHeader("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Physics Simulation", &ctx.sim.physics_enabled);
	}

	ImGui::End();
}

} // namespace ve
