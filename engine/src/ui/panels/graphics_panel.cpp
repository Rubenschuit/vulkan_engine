#include "pch.hpp"
#include "ui/panels/graphics_panel.hpp"
#include "ui/imgui_layer.hpp"
#include "rendering/ve_renderer.hpp"
#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>

namespace ve {

void GraphicsPanel::render(Registry* /*registry*/, EditorState& /*state*/, UIContext& ctx) {
	if (!ImGui::Begin("Graphics")) {
		ImGui::End();
		return;
	}

	// MSAA slider
	{
		ImGui::Text("MSAA:");
		auto available = m_renderer.getAvailableSampleCounts();
		int current = m_renderer.getCurrentSampleCountInt();

		std::vector<std::string> sample_labels;
		size_t current_index = 0;
		for (size_t i = 0; i < available.size(); i++) {
			sample_labels.push_back(available[i] == 1 ? "Off" : std::to_string(available[i]) + "x");
			if (available[i] == current)
				current_index = i;
		}

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

	// Shadow mode
	ImGui::Text("Shadows:");
	ImGui::PushItemWidth(200.0f);
	int shadow_mode_int = static_cast<int>(ctx.shadow_mode);
	if (ImGui::SliderInt("##shadow_slider", &shadow_mode_int, 0, 3, ""))
		ctx.shadow_mode = static_cast<ShadowMode>(std::clamp(shadow_mode_int, 0, 3));
	ImGui::PopItemWidth();
	ImGui::SameLine();
	const char* shadow_labels[] = { "Off", "Normal", "PCF", "PCSS" };
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

	// GTAO
	ImGui::Checkbox("GTAO (Ambient Occlusion)", &ctx.gtao_enabled);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Ground Truth Ambient Occlusion:\ndarkens corners, crevices, and contact areas.\nRequires depth pre-pass enabled.");
	if (ctx.gtao_enabled) {
		ImGui::Indent();
		ImGui::Checkbox("Half Resolution##gtao", &ctx.gtao_half_res);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Compute AO at half screen resolution.\n4x fewer compute invocations, bilinear upsampled.");
		ImGui::SliderFloat("AO Radius", &ctx.gtao_radius, 0.1f, 3.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("World-space sampling radius for AO.\nLarger = broader occlusion, smaller = finer detail.");
		ImGui::SliderFloat("AO Intensity", &ctx.gtao_intensity, 0.5f, 5.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Power curve applied to AO.\nHigher = darker/stronger occlusion effect.");
		ImGui::Unindent();
	}

	// Topology (writes to context only; engine handles pipeline recreation)
	ImGui::Text("Topology: ");
	ImGui::SameLine();
	int topology_int = static_cast<int>(ctx.topology);
	if (ImGui::RadioButton("Triangle List", &topology_int, static_cast<int>(Topology::TRIANGLE_LIST)))
		ctx.topology = Topology::TRIANGLE_LIST;
	ImGui::SameLine();
	if (ImGui::RadioButton("Line List", &topology_int, static_cast<int>(Topology::LINE_LIST)))
		ctx.topology = Topology::LINE_LIST;

	ImGui::Separator();
	ImGui::Text("Render mode (pbr): ");
	int current_render_mode = static_cast<int>(ctx.render_mode);
	if (ImGui::RadioButton("BRDF Microfacets", &current_render_mode, static_cast<int>(RenderMode::BRDF_MICROFACET)))
		ctx.render_mode = RenderMode::BRDF_MICROFACET;
	ImGui::SameLine();
	if (ImGui::RadioButton("BRDF Smooth", &current_render_mode, static_cast<int>(RenderMode::BRDF)))
		ctx.render_mode = RenderMode::BRDF;
	if (ImGui::RadioButton("Normal vector", &current_render_mode, static_cast<int>(RenderMode::NORMAL_VECTOR)))
		ctx.render_mode = RenderMode::NORMAL_VECTOR;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize surface normals (RGB = XYZ)");
	if (ImGui::RadioButton("Tangent vector", &current_render_mode, static_cast<int>(RenderMode::TANGENT_VECTOR)))
		ctx.render_mode = RenderMode::TANGENT_VECTOR;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize tangent vectors for normal mapping");
	if (ImGui::RadioButton("Bitangent vector", &current_render_mode, static_cast<int>(RenderMode::BITANGENT_VECTOR)))
		ctx.render_mode = RenderMode::BITANGENT_VECTOR;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize bitangent vectors for normal mapping");
	if (ImGui::RadioButton("Normal map", &current_render_mode, static_cast<int>(RenderMode::NORMAL_MAP)))
		ctx.render_mode = RenderMode::NORMAL_MAP;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Display normal map texture data directly");
	if (ImGui::RadioButton("CSM Cascades", &current_render_mode, static_cast<int>(RenderMode::CSM_CASCADE)))
		ctx.render_mode = RenderMode::CSM_CASCADE;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize CSM cascade regions\nRed=0, Green=1, Blue=2, Yellow=3");
	if (ImGui::RadioButton("Cluster Heatmap", &current_render_mode, static_cast<int>(RenderMode::CLUSTER_HEATMAP)))
		ctx.render_mode = RenderMode::CLUSTER_HEATMAP;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize lights-per-cluster as a heat gradient\nBlue=few, Red=many, Dark=zero");
	if (ImGui::RadioButton("LOD Level", &current_render_mode, static_cast<int>(RenderMode::LOD_LEVEL)))
		ctx.render_mode = RenderMode::LOD_LEVEL;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize mesh LOD levels\nGreen=0, Yellow=1, Orange=2, Red=3");

	ImGui::Checkbox("Show Axes", &ctx.show_axes);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Display XYZ coordinate axes in the scene.\nRed=X, Green=Y, Blue=Z");
	ImGui::Checkbox("Show AABB outlines", &ctx.show_aabb_debug);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Display wireframe bounding boxes for visible objects");

	ImGui::Separator();
	ImGui::Checkbox("Frustum culling", &ctx.enable_frustum_culling);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Skip drawing objects outside the camera view");
	ImGui::Checkbox("Depth Pre-Pass", &ctx.depth_prepass_enabled);
	ImGui::Checkbox("GPU Culling", &ctx.gpu_culling_enabled);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("GPU-driven frustum culling via compute shader");
	if (ctx.gpu_culling_enabled) {
		ImGui::Indent();
		ImGui::Checkbox("Hi-Z Occlusion Culling", &ctx.hiz_occlusion_enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Prev-frame Hi-Z occlusion culling.\nSkips objects occluded by previous frame's depth.\nRequires depth pre-pass enabled.");
		ImGui::Unindent();
	}
	ImGui::Checkbox("Clustered Lighting", &ctx.cluster_enabled);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Use 3D cluster grid to cull lights per-fragment");

	ImGui::Separator();
	ImGui::Text("Multi-threading:");
	ImGui::SliderInt("Min parallel groups", &ctx.min_parallel_groups, 0, 2048);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Minimum opaque draw groups to enable parallel command recording.\n0 = always parallel");
	ImGui::SliderInt("Min cull entities", &ctx.min_parallel_cull_entities, 0, 4096);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Minimum mesh entities to enable parallel frustum culling.\n0 = always parallel");

	ImGui::Separator();
	ImGui::Text("LOD:");
	{
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

	ImGui::Separator();
	ImGui::Text("Post Processing: ");
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

	ImGui::Text("Bloom");
	ImGui::Separator();
	ImGui::Checkbox("Bloom Enabled", &ctx.bloom_enabled);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Bloom effect creates glow around bright areas.\nSimulates light bleeding in cameras");
	ImGui::DragFloat("Bloom Strength", &ctx.bloom_strength, 0.001f, 0.0f, 0.4f);

	// --- Display ---
	ImGui::Separator();
	ImGui::Text("Display");
	ImGui::Separator();

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

	ImGui::End();
}

} // namespace ve
