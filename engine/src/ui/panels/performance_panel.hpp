#pragma once
#include "ui/editor_panel.hpp"
#include "ui/editor_state.hpp"
#include <chrono>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;

namespace ve {

class VeRenderer;

class VENGINE_API PerformancePanel : public EditorPanel {
public:
	explicit PerformancePanel(VeRenderer& renderer);


	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Performance"; }

private:
	VeRenderer& m_renderer;
	std::string m_gpu_name;

	// Timing accumulators
	std::chrono::high_resolution_clock::time_point m_last_time{};
	bool m_timing_initialized = false;
	int m_frame_count = 0;
	float m_fps = 0.0f;
	float m_frame_time_ms = 0.0f;
	float m_cpu_time_ms = 0.0f;
	float m_fence_wait_ms = 0.0f;
	float m_acquire_wait_ms = 0.0f;
	float m_gpu_time_ms = 0.0f;
	float m_compute_gpu_time_ms = 0.0f;
	float m_gpu_overlap_ms = 0.0f;
	float m_cpu_time_sum = 0.0f;
	float m_fence_wait_sum = 0.0f;
	float m_acquire_wait_sum = 0.0f;
	float m_gpu_time_sum = 0.0f;
	float m_compute_gpu_time_sum = 0.0f;
	float m_gpu_overlap_sum = 0.0f;
	float m_accumulated_dt = 0.0f;
	float m_frametime_history[120] = {};
	int m_history_offset = 0;
	float m_graph_update_timer = 0.0f;
	int m_graph_frames = 0;

	// Per-system breakdown
	static constexpr int BREAKDOWN_COUNT = 8;
	float m_gpu_breakdown_ms[BREAKDOWN_COUNT] = {};
	float m_gpu_breakdown_sum[BREAKDOWN_COUNT] = {};
	float m_cpu_breakdown_ms[BREAKDOWN_COUNT] = {};
	float m_cpu_breakdown_sum[BREAKDOWN_COUNT] = {};

	// VRAM stats
	VmaAllocator m_allocator;
	uint32_t m_heap_count;
	uint32_t m_device_local_heap_index = 0;
};

} // namespace ve
