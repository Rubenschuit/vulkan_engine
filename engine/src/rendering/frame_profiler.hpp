#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <array>
#include <chrono>

namespace ve {

class VeDevice;

enum class ProfileTimer : uint32_t {
	FRAME_TOTAL = 0,
	CULLING,
	SHADOW_MAPS,
	DEPTH_PREPASS,
	GTAO,
	SCENE_RENDER,
	BLOOM,
	POST_PROCESS,
	COMPUTE_TOTAL,
	PHYSICS,
	COUNT
};

inline const char* profileTimerName(ProfileTimer timer) {
	switch (timer) {
		case ProfileTimer::FRAME_TOTAL:    return "Frame Total";
		case ProfileTimer::CULLING:        return "Culling";
		case ProfileTimer::SHADOW_MAPS:    return "Shadows";
		case ProfileTimer::DEPTH_PREPASS:  return "Depth Pass";
		case ProfileTimer::GTAO:           return "GTAO";
		case ProfileTimer::SCENE_RENDER:   return "Scene";
		case ProfileTimer::BLOOM:          return "Bloom";
		case ProfileTimer::POST_PROCESS:   return "Post Process";
		case ProfileTimer::COMPUTE_TOTAL:  return "Compute Total";
		case ProfileTimer::PHYSICS:        return "Physics";
		default:                           return "Unknown";
	}
}

struct ProfileResults {
	static constexpr size_t COUNT = static_cast<size_t>(ProfileTimer::COUNT);

	std::array<float, COUNT> gpu_ms{};
	std::array<float, COUNT> cpu_ms{};
	float gpu_overlap = 0.0f;
	float fence_wait_ms = 0.0f;
	float acquire_wait_ms = 0.0f;

	float gpu(ProfileTimer t) const { return gpu_ms[static_cast<size_t>(t)]; }
	float cpu(ProfileTimer t) const { return cpu_ms[static_cast<size_t>(t)]; }
};

class VENGINE_API FrameProfiler {
public:
	explicit FrameProfiler(VeDevice& device);
	~FrameProfiler() = default;

	FrameProfiler(const FrameProfiler&) = delete;
	FrameProfiler& operator=(const FrameProfiler&) = delete;

	void setGpuProfilingEnabled(bool enabled) { m_gpu_enabled = enabled; }
	bool isGpuProfilingEnabled() const { return m_gpu_enabled; }

	void beginFrame(uint32_t frame_index);
	void beginGpuTimer(vk::raii::CommandBuffer& cmd, ProfileTimer timer);
	void endGpuTimer(vk::raii::CommandBuffer& cmd, ProfileTimer timer);
	void beginCpuTimer(ProfileTimer timer);
	void endCpuTimer(ProfileTimer timer);
	void recordFenceWait(float ms) { m_results.fence_wait_ms = ms; }
	void recordAcquireWait(float ms) { m_results.acquire_wait_ms = ms; }
	void resetAllQueries(vk::raii::CommandBuffer& graphics_cmd,
						 vk::raii::CommandBuffer& compute_cmd,
						 uint32_t frame_index);

	const ProfileResults& getResults() const { return m_results; }

private:
	static constexpr uint32_t TIMER_COUNT = static_cast<uint32_t>(ProfileTimer::COUNT);
	static constexpr uint32_t QUERIES_PER_FRAME = TIMER_COUNT * 2;

	uint32_t queryIndex(uint32_t frame_index, ProfileTimer timer, bool is_end) const {
		return frame_index * QUERIES_PER_FRAME
			 + static_cast<uint32_t>(timer) * 2
			 + (is_end ? 1 : 0);
	}

	static bool isTotalTimer(ProfileTimer timer) {
		return timer == ProfileTimer::FRAME_TOTAL || timer == ProfileTimer::COMPUTE_TOTAL;
	}

	VeDevice& m_device;
	vk::raii::QueryPool m_query_pool{nullptr};
	float m_ticks_to_ms = 0.0f;
	bool m_timestamp_cross_queue = false;
	bool m_gpu_enabled = false;

	std::array<bool, MAX_FRAMES_IN_FLIGHT> m_frame_active{};
	std::array<std::array<bool, TIMER_COUNT>, MAX_FRAMES_IN_FLIGHT> m_gpu_active{};
	uint32_t m_current_frame = 0;

	using clock = std::chrono::steady_clock;
	std::array<clock::time_point, TIMER_COUNT> m_cpu_starts{};

	ProfileResults m_results{};
};

} // namespace ve
