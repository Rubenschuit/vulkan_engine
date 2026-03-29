#include "pch.hpp"
#include "ui/panels/performance_panel.hpp"
#include "ui/imgui_layer.hpp"
#include "rendering/ve_renderer.hpp"
#include <vk_mem_alloc.h>
#include <imgui.h>

namespace ve {

PerformancePanel::PerformancePanel(VeRenderer& renderer)
	: m_renderer(renderer), m_gpu_name(renderer.getDeviceName()),
		m_allocator(renderer.getAllocator()), m_heap_count(renderer.getMemoryHeapCount()) {
	auto mem_props = renderer.getMemoryProperties();
	for (uint32_t i = 0; i < mem_props.memoryHeapCount; i++) {
		if (static_cast<VkMemoryHeapFlags>(mem_props.memoryHeaps[i].flags) & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
			m_device_local_heap_index = i;
			break;
		}
	}
}

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
	if (dt > 1.0f)
		dt = 0.0f; // Clamp: prevents FPS spike after panel re-enable

	m_cpu_time_sum += context.stats.cpu_time;
	m_fence_wait_sum += context.stats.fence_wait;
	m_acquire_wait_sum += context.stats.acquire_wait;
	m_gpu_time_sum += context.stats.gpu_time;
	m_compute_gpu_time_sum += context.stats.compute_gpu_time;
	m_gpu_overlap_sum += context.stats.gpu_overlap;
	m_accumulated_dt += dt;
	m_frame_count++;

	// Per-system breakdown accumulation
	const float gpu_src[] = {
		context.stats.gpu_culling, context.stats.gpu_shadow_maps, context.stats.gpu_depth_prepass,
		context.stats.gpu_gtao, context.stats.gpu_scene_render,
		context.stats.gpu_bloom, context.stats.gpu_post_process, 0.0f
	};
	const float cpu_src[] = {
		context.stats.cpu_culling, context.stats.cpu_shadow_maps,
		context.stats.cpu_depth_prepass, context.stats.cpu_gtao,
		context.stats.cpu_scene_render, context.stats.cpu_bloom,
		context.stats.cpu_post_process, context.stats.cpu_physics
	};
	for (int i = 0; i < BREAKDOWN_COUNT; i++) {
		m_gpu_breakdown_sum[i] += gpu_src[i];
		m_cpu_breakdown_sum[i] += cpu_src[i];
	}

	if (m_frame_count >= 60) {
		m_fps = (m_accumulated_dt > 0.0f) ? (60.0f / m_accumulated_dt) : 0.0f;
		m_frame_time_ms = (m_accumulated_dt / 60.0f) * 1000.0f;
		m_cpu_time_ms = m_cpu_time_sum / 60.0f;
		m_fence_wait_ms = m_fence_wait_sum / 60.0f;
		m_acquire_wait_ms = m_acquire_wait_sum / 60.0f;
		m_gpu_time_ms = m_gpu_time_sum / 60.0f;
		m_compute_gpu_time_ms = m_compute_gpu_time_sum / 60.0f;
		m_gpu_overlap_ms = m_gpu_overlap_sum / 60.0f;

		for (int i = 0; i < BREAKDOWN_COUNT; i++) {
			m_gpu_breakdown_ms[i] = m_gpu_breakdown_sum[i] / 60.0f;
			m_cpu_breakdown_ms[i] = m_cpu_breakdown_sum[i] / 60.0f;
			m_gpu_breakdown_sum[i] = 0.0f;
			m_cpu_breakdown_sum[i] = 0.0f;
		}

		m_frame_count = 0;
		m_cpu_time_sum = 0.0f;
		m_fence_wait_sum = 0.0f;
		m_acquire_wait_sum = 0.0f;
		m_gpu_time_sum = 0.0f;
		m_compute_gpu_time_sum = 0.0f;
		m_gpu_overlap_sum = 0.0f;
		m_accumulated_dt = 0.0f;
	}

	m_graph_update_timer += dt;
	m_graph_frames++;
	if (m_graph_update_timer >= 0.1f) {
		m_frametime_history[m_history_offset] = (m_graph_update_timer / static_cast<float>(m_graph_frames)) * 1000.0f;
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
			ImGui::TextDisabled("CPU %.2f ms | Fence %.2f ms | Acquire %.2f ms | GPU %.2f ms", m_cpu_time_ms, m_fence_wait_ms, m_acquire_wait_ms, m_gpu_time_ms);
			if (m_compute_gpu_time_ms > 0.01f) {
				ImGui::SameLine();
				ImGui::TextDisabled(" | Compute %.2f ms", m_compute_gpu_time_ms);
			}

			char tri_buf[16];
			formatCount(tri_buf, sizeof(tri_buf), context.stats.visible_triangles);
			ImGui::TextDisabled("%s triangles  |  %u/%u objects",
				tri_buf, context.stats.cull_visible_objects, context.stats.cull_total_objects);
			if (context.stats.visible_meshlets > 0) {
				ImGui::SameLine();
				char meshlet_buf[16];
				formatCount(meshlet_buf, sizeof(meshlet_buf), context.stats.visible_meshlets);
				ImGui::TextDisabled("  |  %s meshlets", meshlet_buf);
			}
		}
		ImGui::End();
		return;
	}

	// --- Editor docked panel mode ---
	if (!ImGui::Begin("Performance", &context.show_performance, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	const float col0 = 113.0f;
	auto row = [&](const char* label, const char* value) {
		ImGui::TextDisabled("%s", label);
		ImGui::SameLine(col0);
		ImGui::Text("%s", value);
	};

	// --- Timing ---
	ImGui::Text("%.0f FPS", m_fps);
	ImGui::SameLine();
	ImGui::TextDisabled("%.2f ms (%s)", m_frame_time_ms, m_gpu_name.c_str());
	ImGui::Spacing();

	char val[64];
	snprintf(val, sizeof(val), "%.2f ms", m_cpu_time_ms);
	row("CPU", val);
	snprintf(val, sizeof(val), "%.2f ms", m_fence_wait_ms);
	row("Fence Wait", val);
	snprintf(val, sizeof(val), "%.2f ms", m_acquire_wait_ms);
	row("Acquire Wait", val);
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

	// --- Frame time graph ---
	{
		// Use 95th percentile (skip top 6 of 120) so spikes from
		// mode switches or settings changes don't blow out the Y-axis scale.
		constexpr int SKIP_TOP = 6;
		float top[SKIP_TOP + 1] = {};
		for (float f : m_frametime_history) {
			if (f > top[0]) {
				top[0] = f;
				for (int j = 0; j < SKIP_TOP; j++) {
					if (top[j] > top[j + 1])
						std::swap(top[j], top[j + 1]);
					else
						break;
				}
			}
		}
		float max_ft = top[0]; // largest value not in the top SKIP_TOP
		if (max_ft < 1.0f)
			max_ft = 1.0f;
		float graph_max = max_ft * 1.1f;

		ImGui::PlotLines("##FrameTime", m_frametime_history, 120, m_history_offset,
			nullptr, 0.0f, graph_max, ImVec2(-1, 60));

		ImGui::TextDisabled("0 - %.1f ms", graph_max);
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
	if (context.stats.visible_meshlets > 0) {
		char meshlet_buf[16];
		formatCount(meshlet_buf, sizeof(meshlet_buf), context.stats.visible_meshlets);
		row("Meshlets", meshlet_buf);
	}
	if (context.stats.num_spot_lights > 0)
		snprintf(val, sizeof(val), "%u point, %u dir, %u spot",
			context.stats.num_point_lights, context.stats.num_directional_lights,
			context.stats.num_spot_lights);
	else
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

	// --- VRAM usage (device-local heap only) ---
	{
		VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
		vmaGetHeapBudgets(m_allocator, budgets);

		auto& vram = budgets[m_device_local_heap_index];

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		double used_mb = static_cast<double>(vram.usage) / (1024.0 * 1024.0);
		double budget_mb = static_cast<double>(vram.budget) / (1024.0 * 1024.0);

		snprintf(val, sizeof(val), "%.0f / %.0f MB", used_mb, budget_mb);
		row("VRAM", val);

		float fraction = vram.budget > 0 ? static_cast<float>(static_cast<double>(vram.usage) / static_cast<double>(vram.budget)) : 0.0f;
		ImGui::ProgressBar(fraction, ImVec2(-1, 0));
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Checkbox("GPU Profiling", &context.gpu_profiling);
	ImGui::Spacing();

	// --- Per-system breakdown table ---
	static const char* breakdown_labels[] = {
		"Culling", "Shadows", "Depth Pass", "GTAO", "Scene", "Bloom", "Post Process", "Physics"
	};

	if (ImGui::BeginTable("##Breakdown", 3, ImGuiTableFlags_None)) {
		ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, 65.0f);
		ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_WidthFixed, 65.0f);

		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::TextDisabled("System");
		ImGui::TableNextColumn(); ImGui::TextDisabled("CPU (ms)");
		ImGui::TableNextColumn(); ImGui::TextDisabled("GPU (ms)");

		float cpu_total = 0.0f;
		float gpu_total = 0.0f;
		for (int i = 0; i < BREAKDOWN_COUNT; i++) {
			float cpu = m_cpu_breakdown_ms[i];
			float gpu = m_gpu_breakdown_ms[i];
			cpu_total += cpu;
			gpu_total += gpu;

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%s", breakdown_labels[i]);
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", cpu);
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", gpu);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Separator();
		ImGui::TextDisabled("Total");
		ImGui::TableNextColumn();
		ImGui::Separator();
		ImGui::Text("%.2f", cpu_total);
		ImGui::TableNextColumn();
		ImGui::Separator();
		ImGui::Text("%.2f", gpu_total);

		ImGui::EndTable();
	}

	ImGui::End();
}

} // namespace ve
