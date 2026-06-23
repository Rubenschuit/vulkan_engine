#include "pch.hpp"
#include "ui/panels/debug_panel.hpp"
#include "ui/editor_state.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/texture_inspector.hpp"
#include "rendering/shadow_render_system.hpp"
#include <imgui.h>

namespace ve {

void DebugPanel::render(Registry* /*registry*/, EditorState& state, UIContext& ctx) {
	if (!ImGui::Begin("Debug", &state.show_debug, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	ImGui::SeparatorText("Render Mode");
	int current_render_mode = static_cast<int>(ctx.settings.render_mode);
	if (ImGui::RadioButton("BRDF Microfacets", &current_render_mode, static_cast<int>(RenderMode::BRDF_MICROFACET)))
		ctx.settings.render_mode = RenderMode::BRDF_MICROFACET;
	if (ImGui::RadioButton("BRDF Smooth", &current_render_mode, static_cast<int>(RenderMode::BRDF)))
		ctx.settings.render_mode = RenderMode::BRDF;
	if (ImGui::RadioButton("Normal vector", &current_render_mode, static_cast<int>(RenderMode::NORMAL_VECTOR)))
		ctx.settings.render_mode = RenderMode::NORMAL_VECTOR;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize surface normals (RGB = XYZ)");
	if (ImGui::RadioButton("Tangent vector", &current_render_mode, static_cast<int>(RenderMode::TANGENT_VECTOR)))
		ctx.settings.render_mode = RenderMode::TANGENT_VECTOR;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize tangent vectors for normal mapping");
	if (ImGui::RadioButton("Bitangent vector", &current_render_mode, static_cast<int>(RenderMode::BITANGENT_VECTOR)))
		ctx.settings.render_mode = RenderMode::BITANGENT_VECTOR;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize bitangent vectors for normal mapping");
	if (ImGui::RadioButton("Normal map", &current_render_mode, static_cast<int>(RenderMode::NORMAL_MAP)))
		ctx.settings.render_mode = RenderMode::NORMAL_MAP;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Display normal map texture data directly");
	if (ImGui::RadioButton("CSM Cascades", &current_render_mode, static_cast<int>(RenderMode::CSM_CASCADE)))
		ctx.settings.render_mode = RenderMode::CSM_CASCADE;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize CSM cascade regions\nRed=0, Green=1, Blue=2, Yellow=3");
	if (ImGui::RadioButton("Cluster Heatmap", &current_render_mode, static_cast<int>(RenderMode::CLUSTER_HEATMAP)))
		ctx.settings.render_mode = RenderMode::CLUSTER_HEATMAP;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize lights-per-cluster as a heat gradient\nBlue=few, Red=many, Dark=zero");
	if (ImGui::RadioButton("LOD Level", &current_render_mode, static_cast<int>(RenderMode::LOD_LEVEL)))
		ctx.settings.render_mode = RenderMode::LOD_LEVEL;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize mesh LOD levels\nGreen=0, Yellow=1, Orange=2, Red=3");
	if (ImGui::RadioButton("Meshlet ID", &current_render_mode, static_cast<int>(RenderMode::MESHLET_ID)))
		ctx.settings.render_mode = RenderMode::MESHLET_ID;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Visualize meshlet boundaries (hash-colored per draw command).\nMost useful with meshlet culling enabled.");

	ImGui::SeparatorText("Overlays");
	ImGui::Checkbox("Show Axes", &ctx.settings.show_axes);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Display XYZ coordinate axes in the scene.\nRed=X, Green=Y, Blue=Z");
	ImGui::Checkbox("Show AABB outlines", &ctx.settings.show_aabb_debug);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Display wireframe bounding boxes for visible objects");
	ImGui::Checkbox("Show skinned points", &ctx.settings.show_skinned_points);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Draw post-skin mesh vertices as a colored point cloud.");
	ImGui::Text("Topology");
	ImGui::SameLine();
	int topology_int = static_cast<int>(ctx.settings.topology);
	if (ImGui::RadioButton("Triangle List", &topology_int, static_cast<int>(Topology::TRIANGLE_LIST)))
		ctx.settings.topology = Topology::TRIANGLE_LIST;
	ImGui::SameLine();
	if (ImGui::RadioButton("Line List", &topology_int, static_cast<int>(Topology::LINE_LIST)))
		ctx.settings.topology = Topology::LINE_LIST;

	ImGui::SeparatorText("Textures");
	if (m_shadow_render_system) {
		const VeImage* atlas = m_shadow_render_system->getAtlasImage();
		if (atlas) {
			if (ImGui::Button("Open Shadow Atlas"))
				m_texture_inspector.open(atlas, m_shadow_render_system->getRawSampler(), "Shadow Atlas");
			ImGui::SameLine();
			ImGui::TextDisabled("%ux%u", m_shadow_render_system->getAtlasWidth(), m_shadow_render_system->getAtlasHeight());
		}
	}

	ImGui::End();
}

} // namespace ve
