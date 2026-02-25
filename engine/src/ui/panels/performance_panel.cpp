#include "pch.hpp"
#include "ui/panels/performance_panel.hpp"
#include "ui/imgui_layer.hpp"
#include "rendering/ve_renderer.hpp"
#include <imgui.h>

namespace ve {

static void formatCount(char* buf, size_t buf_size, uint32_t count) {
	if (count >= 1000000)
		snprintf(buf, buf_size, "%.1fM", static_cast<double>(count) / 1000000.0);
	else if (count >= 1000)
		snprintf(buf, buf_size, "%.1fK", static_cast<double>(count) / 1000.0);
	else
		snprintf(buf, buf_size, "%u", count);
}

void PerformancePanel::render(Registry* /*registry*/, EditorState& state, UIContext& context) {
	if (!context.show_performance)
		return;

	// --- Accumulate timing data (runs every frame regardless of display) ---
	auto now = std::chrono::high_resolution_clock::now();
	if (!m_timing_initialized) {
		m_last_time = now;
		m_timing_initialized = true;
	}
	float dt = std::chrono::duration<float, std::chrono::seconds::period>(now - m_last_time).count();
	m_last_time = now;

	m_cpu_time_sum += context.stats.cpu_time;
	m_gpu_time_sum += context.stats.gpu_time;
	m_compute_gpu_time_sum += context.stats.compute_gpu_time;
	m_gpu_overlap_sum += context.stats.gpu_overlap;
	m_accumulated_dt += dt;
	m_frame_count++;

	if (m_frame_count >= 60) {
		m_fps = (m_accumulated_dt > 0.0f) ? (60.0f / m_accumulated_dt) : 0.0f;
		m_frame_time_ms = (m_accumulated_dt / 60.0f) * 1000.0f;
		m_cpu_time_ms = m_cpu_time_sum / 60.0f;
		m_gpu_time_ms = m_gpu_time_sum / 60.0f;
		m_compute_gpu_time_ms = m_compute_gpu_time_sum / 60.0f;
		m_gpu_overlap_ms = m_gpu_overlap_sum / 60.0f;
		m_frame_count = 0;
		m_cpu_time_sum = 0.0f;
		m_gpu_time_sum = 0.0f;
		m_compute_gpu_time_sum = 0.0f;
		m_gpu_overlap_sum = 0.0f;
		m_accumulated_dt = 0.0f;
	}

	m_graph_update_timer += dt;
	m_graph_frames++;
	if (m_graph_update_timer >= 0.1f) {
		m_fps_history[m_history_offset] = static_cast<float>(m_graph_frames) / m_graph_update_timer;
		m_history_offset = (m_history_offset + 1) % 120;
		m_graph_update_timer = 0.0f;
		m_graph_frames = 0;
	}

	// --- Fullscreen overlay mode ---
	if (!state.editor_mode) {
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		float padding = 10.0f;
		ImGui::SetNextWindowPos(
			ImVec2(vp->WorkPos.x + vp->WorkSize.x - padding, vp->WorkPos.y + padding),
			ImGuiCond_Always, ImVec2(1.0f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		ImGui::SetNextWindowSize(ImVec2(0, 0)); // auto-size
		ImGuiWindowFlags overlay_flags =
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove;

		if (ImGui::Begin("##PerfOverlay", &context.show_performance, overlay_flags)) {
			ImGui::Text("%.0f FPS  (%.2f ms)", m_fps, m_frame_time_ms);
			ImGui::TextDisabled("CPU %.2f ms | GPU %.2f ms", m_cpu_time_ms, m_gpu_time_ms);
			if (m_compute_gpu_time_ms > 0.01f) {
				ImGui::SameLine();
				ImGui::TextDisabled(" | Compute %.2f ms", m_compute_gpu_time_ms);
			}

			char tri_buf[16];
			formatCount(tri_buf, sizeof(tri_buf), context.stats.visible_triangles);
			ImGui::TextDisabled("%s triangles  |  %u/%u objects",
				tri_buf, context.stats.cull_visible_objects, context.stats.cull_total_objects);
		}
		ImGui::End();
		return;
	}

	// --- Editor docked panel mode ---
	if (!ImGui::Begin("Performance", &context.show_performance)) {
		ImGui::End();
		return;
	}

	const float col0 = 95.0f;
	auto row = [&](const char* label, const char* value) {
		ImGui::TextDisabled("%s", label);
		ImGui::SameLine(col0);
		ImGui::Text("%s", value);
	};

	// --- Timing ---
	ImGui::Text("%.0f FPS", m_fps);
	ImGui::SameLine();
	ImGui::TextDisabled("%.2f ms", m_frame_time_ms);
	ImGui::Spacing();

	char val[64];
	snprintf(val, sizeof(val), "%.2f ms", m_cpu_time_ms);
	row("CPU", val);
	snprintf(val, sizeof(val), "%.2f ms", m_gpu_time_ms);
	row("GPU Graphics", val);
	if (m_compute_gpu_time_ms > 0.01f) {
		snprintf(val, sizeof(val), "%.2f ms", m_compute_gpu_time_ms);
		row("GPU Compute", val);
		if (m_gpu_overlap_ms > 0.001f) {
			snprintf(val, sizeof(val), "%.2f ms", m_gpu_overlap_ms);
			row("Overlap", val);
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// --- FPS graph ---
	{
		float max_fps = 0.0f;
		for (float f : m_fps_history)
			if (f > max_fps)
				max_fps = f;
		if (max_fps < 60.0f)
			max_fps = 60.0f;
		float graph_max = max_fps * 1.1f;

		// Graph with no overlay text — FPS is already shown above
		ImGui::PlotLines("##FPS", m_fps_history, 120, m_history_offset,
			nullptr, 0.0f, graph_max, ImVec2(-1, 60));

		// Y-axis range labels
		ImGui::TextDisabled("0 - %.0f FPS", graph_max);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// --- Scene stats ---
	char tri_buf[16];
	formatCount(tri_buf, sizeof(tri_buf), context.stats.visible_triangles);
	uint32_t culled = (context.stats.cull_total_objects >= context.stats.cull_visible_objects)
		? (context.stats.cull_total_objects - context.stats.cull_visible_objects) : 0;

	snprintf(val, sizeof(val), "%u / %u (%u culled)",
		context.stats.cull_visible_objects, context.stats.cull_total_objects, culled);
	row("Objects", val);
	row("Triangles", tri_buf);
	snprintf(val, sizeof(val), "%u point, %u dir",
		context.stats.num_point_lights, context.stats.num_directional_lights);
	row("Lights", val);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// --- Renderer info ---
	auto extent = m_renderer.getExtent();
	auto sample_count = static_cast<int>(m_renderer.getSampleCount());

	snprintf(val, sizeof(val), "%u x %u", extent.width, extent.height);
	row("Resolution", val);

	if (sample_count > 1) {
		snprintf(val, sizeof(val), "%dx", sample_count);
		row("MSAA", val);
	}

	if (m_renderer.isHdrEnabled())
		row("HDR", "Enabled");

	ImGui::End();
}

} // namespace ve
