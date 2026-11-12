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
#include "ui/imgui_helpers.hpp"
#include <imgui.h>
#include <algorithm>
#include <format>

namespace ve {

namespace {

// A render mode, or a Radiance Cascades view
struct DebugView {
	const char* group = nullptr; 
	const char* label = nullptr;
	RenderMode mode = RenderMode::BRDF_MICROFACET;
	RcDebugView rc_view = RcDebugView::NONE;
	const char* legend = nullptr;
};

constexpr DebugView DEBUG_VIEWS[] = {
	{.group = "Shading", .label = "Lit", .mode = RenderMode::BRDF_MICROFACET},
	{.label = "Lit (No Normal Maps)", .mode = RenderMode::BRDF,
		.legend = "Full shading with the vertex normal in place of the normal map"},
	{.group = "Geometry", .label = "Normal Vector", .mode = RenderMode::NORMAL_VECTOR,
		.legend = "World-space vertex normal, RGB = XYZ * 0.5 + 0.5"},
	{.label = "Tangent Vector", .mode = RenderMode::TANGENT_VECTOR},
	{.label = "Bitangent Vector", .mode = RenderMode::BITANGENT_VECTOR},
	{.label = "Normal Map", .mode = RenderMode::NORMAL_MAP,
		.legend = "World-space normal-mapped normal, RGB = XYZ * 0.5 + 0.5"},
	{.label = "LOD Level", .mode = RenderMode::LOD_LEVEL,
		.legend = "Green=lod0, Yellow=1, Orange=2, Red=3"},
	{.label = "Meshlet ID", .mode = RenderMode::MESHLET_ID,
		.legend = "One color per meshlet, never shared by meshlets that touch.\nOther culling backends: one color per object."},
	{.group = "Lighting", .label = "CSM Cascades", .mode = RenderMode::CSM_CASCADE,
		.legend = "Red=0, Green=1, Blue=2"},
	{.label = "Cluster Heatmap", .mode = RenderMode::CLUSTER_HEATMAP,
		.legend = "Lights per cluster, log scale: grey = 0, blue = 1, teal = 4, green = 8,\nyellow = 16, orange = 32, red = 64\nAt the cap: magenta and yellow pixel checkerboard"},
	{.group = "Radiance Cascades", .label = "Irradiance", .rc_view = RcDebugView::IRRADIANCE,
		.legend = "The gather's rgb output"},
	{.label = "Sky Visibility", .rc_view = RcDebugView::SKY_VISIBILITY,
		.legend = "The gather's sky visibility around the surface normal, white = open sky"},
	{.label = "Upsample Match", .rc_view = RcDebugView::UPSAMPLE_MATCH,
		.legend = "How well the four nearest GI samples' surfaces match each pixel; black = none, plain bilinear"},
	{.label = "Evidence", .rc_view = RcDebugView::EVIDENCE,
		.legend = "The known share of directions along the specular lobe"},
	{.label = "Probe Radiance", .rc_view = RcDebugView::PROBE_J,
		.legend = "Nearest probe's mean stored radiance at the chosen cascade"},
	{.label = "Trace Outcome", .rc_view = RcDebugView::TRACE_KIND,
		.legend = "First ray, r=hit g=clear/sky b=unknown"},
	{.label = "Convergence", .rc_view = RcDebugView::CONVERGENCE,
		.legend = "Sample count of the probes the pixel gathers from, over the History Length"},
	{.label = "Probe Grid", .rc_view = RcDebugView::PROBE_GRID,
		.legend = "Hash-colored by the pixel's nearest probe at the chosen cascade; black = no probe there"},
	{.label = "Rough Specular", .rc_view = RcDebugView::SPEC,
		.legend = "The radiance RC adds along the specular lobe"},
};

} // namespace

void DebugPanel::render(Registry* /*registry*/, EditorState& state, UIContext& ctx) {
	if (!ImGui::Begin("Debug", &state.show_debug, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	RcSettings& rc = ctx.settings.rc;
	const DebugView* current = nullptr;
	for (const DebugView& view : DEBUG_VIEWS)
		if (rc.debug_view != RcDebugView::NONE
		        ? view.rc_view == rc.debug_view
		        : view.rc_view == RcDebugView::NONE && view.mode == ctx.settings.render_mode)
			current = &view;

	if (ImGui::BeginCombo("View", current ? current->label : "Other", ImGuiComboFlags_HeightLargest)) {
		for (const DebugView& view : DEBUG_VIEWS) {
			if (view.rc_view != RcDebugView::NONE && !m_rc_system)
				continue;
			if (view.group)
				ImGui::SeparatorText(view.group);
			if (ImGui::Selectable(view.label, &view == current)) {
				rc.debug_view = view.rc_view;
				if (view.rc_view == RcDebugView::NONE)
					ctx.settings.render_mode = view.mode;
			}
			if (&view == current)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	if (current && current->legend) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", current->legend);
		ImGui::PopTextWrapPos();
	}
	if (m_rc_system && (rc.debug_view == RcDebugView::PROBE_J || rc.debug_view == RcDebugView::PROBE_GRID))
		ImGui::SliderInt("Probe Cascade", &rc.probe_j_cascade, 0,
			static_cast<int>(m_rc_system->activeCascades() - 1));
	bool line_topology = ctx.settings.topology == Topology::LINE_LIST;
	if (ImGui::Checkbox("Line Topology", &line_topology))
		ctx.settings.topology = line_topology ? Topology::LINE_LIST : Topology::TRIANGLE_LIST;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Draws the triangle indices as a line list, joining index pairs: a rough look at the mesh, not a true wireframe");

	if (m_rc_system && ImGui::CollapsingHeader("Radiance Cascades", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Single Frame", &rc.single_frame);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Show one frame of rays, with no accumulation");
		ImGui::SameLine();
		if (ImGui::Button("Reset Store"))
			m_rc_system->invalidateStore();
		ImGui::Text("Probe Store: %.0f MiB%s%s", double(m_rc_system->storeBytes()) / (1024.0 * 1024.0),
			m_rc_system->storeClampedByDevice() ? " (capped by device limits)" : "",
			m_rc_system->isAllocated() ? "" : " (not allocated)");
		float count_w = 0.0f;
		for (uint32_t n = 0; n < m_rc_system->activeCascades(); n++) {
			const std::string widest = std::format("{0} / {0}", m_rc_system->cascadeCap(n));
			count_w = std::max(count_w, ImGui::CalcTextSize(widest.c_str()).x);
		}
		if (ImGui::BeginTable("##Cascades", 3, ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("##cascade", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("c0").x);
			ImGui::TableSetupColumn("##count", ImGuiTableColumnFlags_WidthFixed, count_w);
			ImGui::TableSetupColumn("##fill", ImGuiTableColumnFlags_WidthStretch);
			for (uint32_t n = 0; n < m_rc_system->activeCascades(); n++) {
				uint32_t cap = m_rc_system->cascadeCap(n);
				uint32_t used = m_rc_system->cascadeProbeCount(n);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("c%u", n);
				ImGui::TableNextColumn();
				ui::textRight("%u / %u", used, cap);
				ImGui::TableNextColumn();
				ImGui::ProgressBar(cap ? float(used) / float(cap) : 0.0f, ImVec2(-1.0f, 0.0f), "");
			}
			ImGui::EndTable();
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
		} else if (!rc.ray_query) {
			ImGui::Text("Tracer: screen-space (Ray Query off)");
		} else {
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
				"Tracer: screen-space (no TLAS: %u BLAS, %u instances)",
				m_ray_tracing_scene->getBlasCount(), m_ray_tracing_scene->getInstanceCount());
		}
		if (m_rc_system->overflowCount() > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
				"Overflow: %u (probes dropped)", m_rc_system->overflowCount());
		// Shares over a window of frames: per frame they jitter in the last digit
		if (m_ray_sums.empty()) {
			m_ray_sums.assign(rcw::RAY_EXIT_CODES, 0);
			m_ray_shares.assign(rcw::RAY_EXIT_CODES, 0.0f);
		}
		for (uint32_t code = 0; code < rcw::RAY_EXIT_CODES; code++)
			m_ray_sums[code] += m_rc_system->rayExitCount(code);
		if (++m_ray_frames >= RAY_SHARE_FRAMES) {
			uint64_t total = 0;
			for (uint64_t sum : m_ray_sums)
				total += sum;
			for (uint32_t code = 0; code < rcw::RAY_EXIT_CODES; code++)
				m_ray_shares[code] = total ? 100.0f * static_cast<float>(m_ray_sums[code]) / static_cast<float>(total) : 0.0f;
			std::fill(m_ray_sums.begin(), m_ray_sums.end(), 0);
			m_ray_frames = 0;
		}
		static const struct { const char* label; uint32_t code; } ray_kinds[] = {
			{"Hit", rcw::RAY_HIT}, {"Clear", rcw::RAY_CLEAR}, {"Sky", rcw::RAY_SKY}, {"Unknown", rcw::RAY_UNKNOWN},
		};
		const float share_w = ImGui::CalcTextSize("100.0%").x;
		if (ImGui::BeginTable("##RayShares", 1 + static_cast<int>(std::size(ray_kinds)), ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("##rays", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Rays").x);
			for (const auto& kind : ray_kinds)
				ImGui::TableSetupColumn(kind.label, ImGuiTableColumnFlags_WidthFixed,
					std::max(share_w, ImGui::CalcTextSize(kind.label).x));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			for (const auto& kind : ray_kinds) {
				ImGui::TableNextColumn();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				ui::textRight("%s", kind.label);
				ImGui::PopStyleColor();
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("Rays");
			for (const auto& kind : ray_kinds) {
				ImGui::TableNextColumn();
				ui::textRight("%.1f%%", m_ray_shares[kind.code]);
			}
			ImGui::EndTable();
		}
		ImGui::SetItemTooltip("Share of cascade 0 rays by outcome, averaged over %d frames", RAY_SHARE_FRAMES);
	}

	if (ImGui::CollapsingHeader("Overlays", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Axes", &ctx.settings.show_axes);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Red=X, Green=Y, Blue=Z");
		ImGui::Checkbox("AABB Outlines", &ctx.settings.show_aabb_debug);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Frustum-visible objects; CPU culling backend only.");
		ImGui::Checkbox("Skinned Points", &ctx.settings.show_skinned_points);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Draw post-skin mesh vertices as a colored point cloud.");
		ImGui::Checkbox("Area Light Gizmos", &ctx.settings.show_area_lights);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Draw wireframe quads + facing normal for area lights");
	}

	if (ImGui::CollapsingHeader("Tools", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (m_fly_camera) {
			glm::vec3 pos = m_fly_camera->position();
			glm::vec3 look = pos + m_fly_camera->forward() * 10.0f;
			std::string pose = std::format("{:.2f},{:.2f},{:.2f}:{:.2f},{:.2f},{:.2f}",
				pos.x, pos.y, pos.z, look.x, look.y, look.z);
			if (ImGui::Button("Copy --bench-camera"))
				ImGui::SetClipboardText(std::format("--bench-camera \"{}\"", pose).c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Copy the current editor camera pose as a VeApp benchmark flag");
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("%s", pose.c_str());
			ImGui::PopTextWrapPos();
		}
		if (m_shadow_render_system && m_shadow_render_system->getAtlasImage()) {
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
