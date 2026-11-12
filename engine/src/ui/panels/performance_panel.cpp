#include "pch.hpp"
#include "ui/panels/performance_panel.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/imgui_helpers.hpp"
#include "rendering/ve_renderer.hpp"
#include <vk_mem_alloc.h>
#include <imgui.h>

namespace ve {

namespace {

// A breakdown row, in roughly the order the frame runs them. A null stat has no timer on that side.
struct TimingRow {
	const char* label;
	float FrameStats::* cpu = nullptr;
	float FrameStats::* gpu = nullptr;
	int depth = 0;
	bool other = false; // adds an "Other" sub-row with the GPU time its sub-rows leave untimed
};

constexpr TimingRow TIMING_ROWS[] = {
	{.label = "Physics", .cpu = &FrameStats::cpu_physics},
	{.label = "Skinning", .cpu = &FrameStats::cpu_skinning, .gpu = &FrameStats::gpu_skinning},
	{.label = "Particles", .cpu = &FrameStats::cpu_particles, .gpu = &FrameStats::gpu_particles},
	{.label = "Cluster Lights", .cpu = &FrameStats::cpu_cluster_lights, .gpu = &FrameStats::gpu_cluster_lights},
	{.label = "Culling", .cpu = &FrameStats::cpu_culling, .gpu = &FrameStats::gpu_culling},
	{.label = "Hi-Z", .cpu = &FrameStats::cpu_hiz, .gpu = &FrameStats::gpu_hiz},
	{.label = "Geometry Pass", .cpu = &FrameStats::cpu_geometry_prepass, .gpu = &FrameStats::gpu_geometry_prepass, .other = true},
	{.label = "Objects", .gpu = &FrameStats::gpu_prepass_objects, .depth = 1},
	{.label = "Shadows", .cpu = &FrameStats::cpu_shadow_maps, .gpu = &FrameStats::gpu_shadow_maps},
	{.label = "Shadow Mask", .cpu = &FrameStats::cpu_shadow_mask, .gpu = &FrameStats::gpu_shadow_mask},
	{.label = "GTAO", .cpu = &FrameStats::cpu_gtao, .gpu = &FrameStats::gpu_gtao},
	{.label = "SSR", .cpu = &FrameStats::cpu_ssr, .gpu = &FrameStats::gpu_ssr},
	{.label = "RC GI", .cpu = &FrameStats::cpu_rc, .gpu = &FrameStats::gpu_rc},
	{.label = "Accel. Structures", .cpu = &FrameStats::cpu_rc_as, .gpu = &FrameStats::gpu_rc_as, .depth = 1},
	{.label = "Build", .gpu = &FrameStats::gpu_rc_build, .depth = 1},
	{.label = "Map", .gpu = &FrameStats::gpu_rc_map, .depth = 2},
	{.label = "Raygen", .gpu = &FrameStats::gpu_rc_raygen, .depth = 2},
	{.label = "Trace", .gpu = &FrameStats::gpu_rc_trace, .depth = 2},
	{.label = "Resolve", .gpu = &FrameStats::gpu_rc_resolve, .depth = 2},
	{.label = "Shade", .gpu = &FrameStats::gpu_rc_shade, .depth = 1},
	{.label = "Merge", .gpu = &FrameStats::gpu_rc_merge, .depth = 2},
	{.label = "Irradiance", .gpu = &FrameStats::gpu_rc_irradiance, .depth = 2},
	{.label = "Gather", .gpu = &FrameStats::gpu_rc_gather, .depth = 2},
	{.label = "Scene", .cpu = &FrameStats::cpu_scene_render, .gpu = &FrameStats::gpu_scene_render, .other = true},
	{.label = "Objects", .gpu = &FrameStats::gpu_scene_objects, .depth = 1},
	{.label = "Skybox", .gpu = &FrameStats::gpu_scene_skybox, .depth = 1},
	{.label = "Outline", .cpu = &FrameStats::cpu_outline, .gpu = &FrameStats::gpu_outline},
	{.label = "Bloom", .cpu = &FrameStats::cpu_bloom, .gpu = &FrameStats::gpu_bloom},
	{.label = "Post Process", .cpu = &FrameStats::cpu_post_process, .gpu = &FrameStats::gpu_post_process},
	{.label = "UI", .cpu = &FrameStats::cpu_ui, .gpu = &FrameStats::gpu_ui},
};
constexpr size_t TIMING_ROW_COUNT = std::size(TIMING_ROWS);

constexpr float IDLE_MS = 0.005f; // reads as 0.00
constexpr float IDLE_ROW_HIDE_S = 2.0f; // the overlay drops a pass once it has not run for this long
constexpr ImGuiTreeNodeFlags LEAF_ROW = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
	| ImGuiTreeNodeFlags_SpanAllColumns;

} // namespace

PerformancePanel::PerformancePanel(VeRenderer& renderer)
	: m_renderer(renderer), m_gpu_name(renderer.getDeviceName()),
		m_row_sums(TIMING_ROW_COUNT, glm::vec2(0.0f)), m_row_ms(TIMING_ROW_COUNT, glm::vec2(0.0f)),
		m_row_idle_s(TIMING_ROW_COUNT, IDLE_ROW_HIDE_S),
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

// Two label/value pairs per row in fixed columns with right-aligned values, so the layout
// keeps its width as the numbers change
static bool beginPairTable(const char* id, const char* widest_label, const char* widest_value) {
	if (!ImGui::BeginTable(id, 5, ImGuiTableFlags_SizingFixedFit))
		return false;
	const float label_w = ImGui::CalcTextSize(widest_label).x;
	const float value_w = ImGui::CalcTextSize(widest_value).x;
	ImGui::TableSetupColumn("##label_a", ImGuiTableColumnFlags_WidthFixed, label_w);
	ImGui::TableSetupColumn("##value_a", ImGuiTableColumnFlags_WidthFixed, value_w);
	ImGui::TableSetupColumn("##gap", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize());
	ImGui::TableSetupColumn("##label_b", ImGuiTableColumnFlags_WidthFixed, label_w);
	ImGui::TableSetupColumn("##value_b", ImGuiTableColumnFlags_WidthFixed, value_w);
	return true;
}

static void pair(const char* label, const char* tooltip, const char* fmt, ...) {
	ImGui::TableNextColumn();
	ImGui::TextDisabled("%s", label);
	if (tooltip)
		ImGui::SetItemTooltip("%s", tooltip);
	ImGui::TableNextColumn();
	char buf[32];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ui::textRight("%s", buf);
}

// The bar behind a GPU time is its share of the GPU frame time
static void gpuCell(float ms, float frame_ms, bool timed) {
	ImGui::TableNextColumn();
	if (!timed) {
		ui::textRight("-");
		return;
	}
	if (frame_ms > 0.0f && ms > 0.0f) {
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x * std::min(ms / frame_ms, 1.0f);
		ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + ImGui::GetTextLineHeight()),
			ImGui::GetColorU32(ImGuiCol_PlotHistogram, 0.35f));
	}
	ui::textRight("%.2f", ms);
}

void PerformancePanel::overlayPasses(float width, bool gpu_timed) const {
	if (!ImGui::BeginTable("##OverlayPasses", 3, ImGuiTableFlags_SizingFixedFit, ImVec2(width, 0.0f)))
		return;
	const float value_w = ImGui::CalcTextSize("GPU (ms)").x;
	ImGui::TableSetupColumn("##pass", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("##cpu", ImGuiTableColumnFlags_WidthFixed, value_w);
	ImGui::TableSetupColumn("##gpu", ImGuiTableColumnFlags_WidthFixed, value_w);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(1);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ui::textRight("CPU (ms)");
	ImGui::TableNextColumn();
	ui::textRight("GPU (ms)");
	ImGui::PopStyleColor();

	glm::vec2 total(0.0f);
	for (size_t i = 0; i < TIMING_ROW_COUNT; i++) {
		const TimingRow& timing = TIMING_ROWS[i];
		if (timing.depth > 0)
			continue;
		const glm::vec2 ms = m_row_ms[i];
		total += ms;
		if (m_row_idle_s[i] >= IDLE_ROW_HIDE_S)
			continue;
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextDisabled("%s", timing.label);
		ImGui::TableNextColumn();
		if (timing.cpu)
			ui::textRight("%.2f", ms.x);
		if (timing.gpu)
			gpuCell(ms.y, m_gpu_time_ms, gpu_timed);
	}

	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::Separator();
	ImGui::TextDisabled("Total");
	ImGui::TableNextColumn();
	ImGui::Separator();
	ui::textRight("%.2f", total.x);
	ImGui::TableNextColumn();
	ImGui::Separator();
	if (gpu_timed)
		ui::textRight("%.2f", total.y);
	else
		ui::textRight("-");
	ImGui::EndTable();
}

void PerformancePanel::timingRows(bool waits) const {
	ImGui::TableNextRow();
	pair("FPS", nullptr, "%.0f", m_fps);
	ImGui::TableNextColumn();
	pair("Frame", nullptr, "%.2f ms", m_frame_time_ms);
	ImGui::TableNextRow();
	pair("CPU", "Excludes the fence and acquire waits", "%.2f ms", m_cpu_time_ms);
	ImGui::TableNextColumn();
	pair("GPU", nullptr, "%.2f ms", m_gpu_time_ms);
	if (!waits)
		return;
	ImGui::TableNextRow();
	pair("Fence", "Wait for the GPU to free a frame slot", "%.2f ms", m_fence_wait_ms);
	ImGui::TableNextColumn();
	pair("Acquire", "Wait for a swapchain image", "%.2f ms", m_acquire_wait_ms);
}

void PerformancePanel::render(Registry* /*registry*/, EditorState& state, UIContext& context) {
	if (!state.show_performance)
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
	m_accumulated_dt += dt;
	m_frame_count++;

	for (size_t i = 0; i < TIMING_ROW_COUNT; i++) {
		const TimingRow& row = TIMING_ROWS[i];
		m_row_sums[i] += glm::vec2(row.cpu ? context.stats.*row.cpu : 0.0f, row.gpu ? context.stats.*row.gpu : 0.0f);
	}

	if (m_frame_count >= 60) {
		m_fps = (m_accumulated_dt > 0.0f) ? (60.0f / m_accumulated_dt) : 0.0f;
		m_frame_time_ms = (m_accumulated_dt / 60.0f) * 1000.0f;
		m_cpu_time_ms = m_cpu_time_sum / 60.0f;
		m_fence_wait_ms = m_fence_wait_sum / 60.0f;
		m_acquire_wait_ms = m_acquire_wait_sum / 60.0f;
		m_gpu_time_ms = m_gpu_time_sum / 60.0f;

		const bool gpu_timed = context.settings.gpu_profiling;
		for (size_t i = 0; i < TIMING_ROW_COUNT; i++) {
			m_row_ms[i] = m_row_sums[i] / 60.0f;
			m_row_sums[i] = glm::vec2(0.0f);
			const bool idle = m_row_ms[i].x == 0.0f && (!gpu_timed || m_row_ms[i].y == 0.0f);
			m_row_idle_s[i] = idle ? std::min(m_row_idle_s[i] + m_accumulated_dt, IDLE_ROW_HIDE_S) : 0.0f;
		}

		m_frame_count = 0;
		m_cpu_time_sum = 0.0f;
		m_fence_wait_sum = 0.0f;
		m_acquire_wait_sum = 0.0f;
		m_gpu_time_sum = 0.0f;
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

		if (ImGui::Begin("##PerfOverlay", &state.show_performance, overlay_flags)) {
			if (beginPairTable("##Overlay", "Resolution", "0000x0000")) {
				timingRows(false);
				char count_a[16];
				char count_b[16];
				formatCount(count_a, sizeof(count_a), context.stats.visible_triangles);
				formatCount(count_b, sizeof(count_b), context.stats.cull_visible_objects);
				ImGui::TableNextRow();
				pair("Triangles", nullptr, "%s", count_a);
				ImGui::TableNextColumn();
				pair("Objects", nullptr, "%s", count_b);
				ImGui::TableNextRow();
				auto extent = m_renderer.getExtent();
				pair("Resolution", nullptr, "%ux%u", extent.width, extent.height);
				if (context.stats.visible_meshlets > 0) {
					formatCount(count_a, sizeof(count_a), context.stats.visible_meshlets);
					ImGui::TableNextColumn();
					pair("Meshlets", nullptr, "%s", count_a);
				}
				ImGui::EndTable();
				if (state.performance_overlay_passes) {
					const float width = ImGui::GetItemRectSize().x;
					ImGui::Separator();
					overlayPasses(width, context.settings.gpu_profiling);
				}
			}
		}
		ImGui::End();
		return;
	}

	// --- Editor docked panel mode ---
	if (!ImGui::Begin("Performance", &state.show_performance, ImGuiWindowFlags_NoFocusOnAppearing)) {
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
	const char* limiter = m_gpu_time_ms > m_cpu_time_ms ? "GPU-bound" : "CPU-bound";
	if (context.settings.vsync && m_frame_time_ms > 1.2f * std::max(m_cpu_time_ms, m_gpu_time_ms))
		limiter = "VSync-limited";
	ImGui::Text("%s", limiter);
	ImGui::SetItemTooltip("Whichever of the CPU and the GPU takes longer per frame sets the frame rate");
	ImGui::SameLine();
	ImGui::TextDisabled("(%s)", m_gpu_name.c_str());

	if (beginPairTable("##Headline", "Acquire", "000.00 ms")) {
		timingRows(true);
		ImGui::EndTable();
	}
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
		const float max_ft = top[0]; // largest value not in the top SKIP_TOP
		const float step = max_ft < 5.0f ? 1.0f : 5.0f;
		const float graph_max = std::max(step, std::ceil(max_ft * 1.05f / step) * step);

		ImGui::PlotLines("##FrameTime", m_frametime_history, 120, m_history_offset,
			nullptr, 0.0f, graph_max, ImVec2(-1, 60));
		if (ImGui::IsItemHovered()) {
			const float pad_x = ImGui::GetStyle().FramePadding.x;
			const float x0 = ImGui::GetItemRectMin().x + pad_x;
			const float x1 = ImGui::GetItemRectMax().x - pad_x;
			const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 0.9999f);
			const int sample = static_cast<int>(t * 119.0f);
			ImGui::SetTooltip("%.2f ms", m_frametime_history[(sample + m_history_offset) % 120]);
		}
	}

	// --- Per-system breakdown ---
	ImGui::Spacing();
	ImGui::Checkbox("GPU Profiling", &context.settings.gpu_profiling);
	ImGui::SetItemTooltip("Collect per-pass GPU timings");
	const bool gpu_timed = context.settings.gpu_profiling;

	if (ImGui::BeginTable("##Breakdown", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
		ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_WidthFixed, 65.0f);
		ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_WidthFixed, 65.0f);
		ImGui::TableHeadersRow();

		auto pushTextColor = [](bool dim) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text));
		};

		// Draws row i and, when it is open, its sub-rows; returns the index past its sub-rows
		auto drawRow = [&](auto& self, size_t i) -> size_t {
			const TimingRow& timing = TIMING_ROWS[i];
			const glm::vec2 ms = m_row_ms[i];
			size_t end = i + 1;
			while (end < TIMING_ROW_COUNT && TIMING_ROWS[end].depth > timing.depth)
				end++;
			const bool expandable = end > i + 1 && gpu_timed && ms.y > 0.0f;
			const bool idle = ms.x < IDLE_MS && (!gpu_timed || ms.y < IDLE_MS);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			pushTextColor(timing.depth > 0 || idle);
			const bool open = ImGui::TreeNodeEx(timing.label,
				expandable ? ImGuiTreeNodeFlags_SpanAllColumns : LEAF_ROW);
			ImGui::TableNextColumn();
			if (timing.cpu)
				ui::textRight("%.2f", ms.x);
			if (timing.gpu)
				gpuCell(ms.y, m_gpu_time_ms, gpu_timed);
			else
				ImGui::TableNextColumn();
			ImGui::PopStyleColor();

			if (expandable && open) {
				float sub_gpu = 0.0f;
				for (size_t c = i + 1; c < end; ) {
					if (TIMING_ROWS[c].depth == timing.depth + 1)
						sub_gpu += m_row_ms[c].y;
					c = self(self, c);
				}
				if (timing.other) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					pushTextColor(true);
					ImGui::TreeNodeEx("Other", LEAF_ROW);
					ImGui::SetItemTooltip("Clears, resolves and the draws no sub-timer covers");
					ImGui::TableNextColumn();
					gpuCell(std::max(ms.y - sub_gpu, 0.0f), m_gpu_time_ms, gpu_timed);
					ImGui::PopStyleColor();
				}
				ImGui::TreePop();
			}
			return end;
		};

		glm::vec2 total(0.0f);
		for (size_t i = 0; i < TIMING_ROW_COUNT; ) {
			total += m_row_ms[i];
			i = drawRow(drawRow, i);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Separator();
		ImGui::TreeNodeEx("Total", LEAF_ROW);
		ImGui::SetItemTooltip("Sum of the rows above. Passes on the async compute queue overlap graphics work,\nso the GPU sum can exceed the frame time.");
		ImGui::TableNextColumn();
		ImGui::Separator();
		ui::textRight("%.2f", total.x);
		ImGui::TableNextColumn();
		ImGui::Separator();
		if (gpu_timed)
			ui::textRight("%.2f", total.y);
		else
			ui::textRight("-");

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TreeNodeEx("Frame", LEAF_ROW);
		ImGui::SetItemTooltip("Measured per frame. The CPU time excludes the fence and acquire waits.");
		ImGui::TableNextColumn();
		ui::textRight("%.2f", m_cpu_time_ms);
		ImGui::TableNextColumn();
		ui::textRight("%.2f", m_gpu_time_ms);

		ImGui::EndTable();
	}

	// --- Scene stats ---
	ImGui::SeparatorText("Scene");
	char val[64];
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
	int light_len = snprintf(val, sizeof(val), "%u point, %u dir",
		context.stats.num_point_lights, context.stats.num_directional_lights);
	if (context.stats.num_spot_lights > 0)
		light_len += snprintf(val + light_len, sizeof(val) - static_cast<size_t>(light_len), ", %u spot",
			context.stats.num_spot_lights);
	if (context.stats.num_area_lights > 0)
		snprintf(val + light_len, sizeof(val) - static_cast<size_t>(light_len), ", %u area",
			context.stats.num_area_lights);
	row("Lights", val);

	// --- Renderer info ---
	ImGui::SeparatorText("Display");
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
	// Number = sum of live VMA allocations.
	// Bar = driver-reported committed memory (tracks OOM pressure).
	{
		ImGui::SeparatorText("Memory");
		VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
		vmaGetHeapBudgets(m_allocator, budgets);

		auto& vram = budgets[m_device_local_heap_index];

		constexpr double GIB = 1024.0 * 1024.0 * 1024.0;
		double live_gib = static_cast<double>(vram.statistics.allocationBytes) / GIB;
		double budget_gib = static_cast<double>(vram.budget) / GIB;

		snprintf(val, sizeof(val), "%.1f / %.1f GiB", live_gib, budget_gib);
		row("VRAM", val);

		float fraction = vram.budget > 0 ? static_cast<float>(static_cast<double>(vram.usage) / static_cast<double>(vram.budget)) : 0.0f;
		ImGui::ProgressBar(fraction, ImVec2(-1, 0));
	}

	ImGui::End();
}

} // namespace ve
