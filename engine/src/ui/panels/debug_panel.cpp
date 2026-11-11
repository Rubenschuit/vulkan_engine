#include "pch.hpp"
#include "ui/panels/debug_panel.hpp"
#include "scene/fly_camera_controller.hpp"
#include "vulkan/ve_image.hpp"
#include "ui/editor_state.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/texture_inspector.hpp"
#include "rendering/shadow_render_system.hpp"
#include "rendering/rc_system.hpp"
#include "rendering/ray_tracing_scene.hpp"
#include "rendering/rc_world_mirrors.hpp"
#include <imgui.h>
#include <format>

namespace ve {

void DebugPanel::render(Registry* /*registry*/, EditorState& state, UIContext& ctx) {
	if (!ImGui::Begin("Debug", &state.show_debug, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	ImGui::SeparatorText("Render Mode");
	constexpr int RC_VIEW_RADIO_BASE = 100;
	int selected = ctx.settings.rc.debug_view != RcDebugView::NONE
		? RC_VIEW_RADIO_BASE + static_cast<int>(ctx.settings.rc.debug_view)
		: static_cast<int>(ctx.settings.render_mode);
	auto render_mode_radio = [&](const char* label, RenderMode mode, const char* tooltip = nullptr) {
		if (ImGui::RadioButton(label, &selected, static_cast<int>(mode))) {
			ctx.settings.render_mode = mode;
			ctx.settings.rc.debug_view = RcDebugView::NONE;
		}
		if (tooltip && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);
	};
	auto rc_view_radio = [&](const char* label, RcDebugView view, const char* tooltip) {
		if (ImGui::RadioButton(label, &selected, RC_VIEW_RADIO_BASE + static_cast<int>(view)))
			ctx.settings.rc.debug_view = view;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);
	};
	render_mode_radio("Lit", RenderMode::BRDF_MICROFACET);
	render_mode_radio("Lit (No Normal Maps)", RenderMode::BRDF, "Full shading with the vertex normal in place of the normal map");
	render_mode_radio("Normal Vector", RenderMode::NORMAL_VECTOR, "World-space vertex normal, RGB = XYZ * 0.5 + 0.5");
	render_mode_radio("Tangent Vector", RenderMode::TANGENT_VECTOR);
	render_mode_radio("Bitangent Vector", RenderMode::BITANGENT_VECTOR);
	render_mode_radio("Normal Map", RenderMode::NORMAL_MAP, "World-space normal-mapped normal, RGB = XYZ * 0.5 + 0.5");
	render_mode_radio("CSM Cascades", RenderMode::CSM_CASCADE, "Red=0, Green=1, Blue=2");
	render_mode_radio("Cluster Heatmap", RenderMode::CLUSTER_HEATMAP,
		"Lights per cluster, log scale: grey = 0, blue = 1, teal = 4, green = 8,\nyellow = 16, orange = 32, red = 64\nAt the cap: magenta and yellow pixel checkerboard");
	render_mode_radio("LOD Level", RenderMode::LOD_LEVEL, "Green=lod0, Yellow=1, Orange=2, Red=3");
	render_mode_radio("Meshlet ID", RenderMode::MESHLET_ID,
		"Hash-colored by meshlet index within its object.\nRequires the Meshlet culling backend.");
	if (m_rc_system) {
		ImGui::TextDisabled("Radiance Cascades");
		rc_view_radio("Irradiance", RcDebugView::IRRADIANCE, "The gather's rgb output");
		rc_view_radio("Sky Visibility", RcDebugView::SKY_VISIBILITY,
			"The gather's sky visibility along the specular lobe, white = open sky");
		rc_view_radio("Upsample Match", RcDebugView::UPSAMPLE_MATCH,
			"How well the four nearest GI samples' surfaces match each pixel; black = none, plain bilinear");
		rc_view_radio("Evidence", RcDebugView::EVIDENCE, "The known share of directions along the specular lobe");
		rc_view_radio("Probe Radiance", RcDebugView::PROBE_J,
			"Nearest probe's mean stored radiance at the chosen cascade");
		rc_view_radio("Trace Outcome", RcDebugView::TRACE_KIND,
			"First ray, r=hit g=clear/sky b=unknown");
		rc_view_radio("Convergence", RcDebugView::CONVERGENCE,
			"Sample count of the probes the pixel gathers from, over the History Length");
		rc_view_radio("Probe Grid", RcDebugView::PROBE_GRID,
			"Hash-colored by the pixel's nearest probe at the chosen cascade; black = no probe there");
		rc_view_radio("Rough Specular", RcDebugView::SPEC,
			"The radiance RC adds along the specular lobe");
	}

	ImGui::SeparatorText("Overlays");
	ImGui::Checkbox("Show Axes", &ctx.settings.show_axes);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Red=X, Green=Y, Blue=Z");
	ImGui::Checkbox("Show AABB Outlines", &ctx.settings.show_aabb_debug);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Frustum-visible objects; CPU culling backend only.");
	ImGui::Checkbox("Show Skinned Points", &ctx.settings.show_skinned_points);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Draw post-skin mesh vertices as a colored point cloud.");
	ImGui::Checkbox("Show Area Light Gizmos", &ctx.settings.show_area_lights);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Draw wireframe quads + facing normal for area lights");
	ImGui::Text("Topology");
	ImGui::SameLine();
	int topology_int = static_cast<int>(ctx.settings.topology);
	if (ImGui::RadioButton("Triangle List", &topology_int, static_cast<int>(Topology::TRIANGLE_LIST)))
		ctx.settings.topology = Topology::TRIANGLE_LIST;
	ImGui::SameLine();
	if (ImGui::RadioButton("Line List", &topology_int, static_cast<int>(Topology::LINE_LIST)))
		ctx.settings.topology = Topology::LINE_LIST;

	ImGui::SeparatorText("Benchmark Camera");
	if (m_fly_camera) {
		glm::vec3 pos = m_fly_camera->position();
		glm::vec3 look = pos + m_fly_camera->forward() * 10.0f;
		std::string pose = std::format("{:.2f},{:.2f},{:.2f}:{:.2f},{:.2f},{:.2f}",
			pos.x, pos.y, pos.z, look.x, look.y, look.z);
		ImGui::TextWrapped("%s", pose.c_str());
		if (ImGui::Button("Copy --bench-camera"))
			ImGui::SetClipboardText(std::format("--bench-camera \"{}\"", pose).c_str());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Copy the current editor camera pose as a VeApp benchmark flag");
	}

	if (m_rc_system) {
		ImGui::SeparatorText("Radiance Cascades");
		ImGui::Checkbox("Single Frame", &ctx.settings.rc.single_frame);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Show one frame of rays, with no accumulation");
		ImGui::SameLine();
		if (ImGui::Button("Reset Store"))
			m_rc_system->invalidateStore();
		if (ctx.settings.rc.debug_view == RcDebugView::PROBE_J || ctx.settings.rc.debug_view == RcDebugView::PROBE_GRID)
			ImGui::SliderInt("Probe Cascade", &ctx.settings.rc.probe_j_cascade, 0,
				static_cast<int>(m_rc_system->activeCascades() - 1));
		ImGui::Text("Probe Store: %.0f MiB%s%s", double(m_rc_system->storeBytes()) / (1024.0 * 1024.0),
			m_rc_system->storeClampedByDevice() ? " (capped by device limits)" : "",
			m_rc_system->isAllocated() ? "" : " (not allocated)");
		for (uint32_t n = 0; n < m_rc_system->activeCascades(); n++) {
			uint32_t cap = m_rc_system->cascadeCap(n);
			uint32_t used = m_rc_system->cascadeProbeCount(n);
			ImGui::Text("  c%u %5u / %5u", n, used, cap);
			ImGui::SameLine();
			ImGui::ProgressBar(cap ? float(used) / float(cap) : 0.0f, ImVec2(-1.0f, 0.0f), "");
		}
		// Which tracer actually ran, and what is missing when it is not the one asked for.
		if (!m_rc_system->isAllocated()) {
			ImGui::Text("Tracer: none (RC off)");
		} else if (m_rc_system->usingRayQuery()) {
			ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "Tracer: ray query");
			ImGui::Text("  %u BLAS, %u instances, %.0f MiB", m_ray_tracing_scene->getBlasCount(),
				m_ray_tracing_scene->getInstanceCount(),
				double(m_ray_tracing_scene->getStoreBytes()) / (1024.0 * 1024.0));
		} else if (!m_ray_tracing_scene) {
			ImGui::Text("Tracer: screen-space (device has no ray query)");
		} else if (!ctx.settings.rc.ray_query) {
			ImGui::Text("Tracer: screen-space (Ray Query Tracing off)");
		} else {
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
				"Tracer: screen-space (no TLAS: %u BLAS, %u instances)",
				m_ray_tracing_scene->getBlasCount(), m_ray_tracing_scene->getInstanceCount());
		}
		if (m_rc_system->overflowCount() > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
				"overflow %u (probes dropped)", m_rc_system->overflowCount());
		uint32_t ray_total = 0;
		for (uint32_t code = 0; code < rcw::RAY_EXIT_CODES; code++)
			ray_total += m_rc_system->rayExitCount(code);
		if (ray_total > 0) {
			const float k = 100.0f / static_cast<float>(ray_total);
			ImGui::Text("rays: hit %.1f%%  clear %.1f%%  sky %.1f%%  unknown %.1f%%",
				m_rc_system->rayExitCount(rcw::RAY_HIT) * k, m_rc_system->rayExitCount(rcw::RAY_CLEAR) * k,
				m_rc_system->rayExitCount(rcw::RAY_SKY) * k, m_rc_system->rayExitCount(rcw::RAY_UNKNOWN) * k);
		}
	}

	ImGui::SeparatorText("Textures");
	if (m_shadow_render_system) {
		const VeImage* atlas = m_shadow_render_system->getAtlasImage();
		if (atlas) {
			if (ImGui::Button("Open Shadow Atlas"))
				m_texture_inspector.open({
					.image = [this] { return m_shadow_render_system->getAtlasImage(); },
					.generation = [this] { return m_shadow_render_system->getAtlasGeneration(); },
				}, m_shadow_render_system->getRawSampler(), "Shadow Atlas");
			ImGui::SameLine();
			ImGui::TextDisabled("%ux%u", m_shadow_render_system->getAtlasWidth(), m_shadow_render_system->getAtlasHeight());
		}
	}
	ImGui::End();
}

} // namespace ve
