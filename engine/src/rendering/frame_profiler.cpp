#include "pch.hpp"
#include "rendering/frame_profiler.hpp"
#include "vulkan/ve_device.hpp"

namespace ve {

FrameProfiler::FrameProfiler(VeDevice& device) : m_device(device) {
	auto props = m_device.getDeviceProperties();
	auto queue_props = m_device.getPhysicalDevice().getQueueFamilyProperties();
	m_timestamps_supported = !queue_props.empty() && queue_props[0].timestampValidBits > 0;

	if (m_timestamps_supported) {
		vk::QueryPoolCreateInfo pool_info{
			.sType = vk::StructureType::eQueryPoolCreateInfo,
			.queryType = vk::QueryType::eTimestamp,
			.queryCount = QUERIES_PER_FRAME * MAX_FRAMES_IN_FLIGHT
		};
		m_query_pool = vk::raii::QueryPool(m_device.getDevice(), pool_info);
		m_ticks_to_ms = props.limits.timestampPeriod / 1000000.0f;
	} else {
		VE_LOGW("GPU timestamps not supported (timestampValidBits == 0), GPU profiling disabled.");
	}
}

void FrameProfiler::beginFrame(uint32_t frame_index) {
	m_results.cpu_ms.fill(0.0f);

	// Resolve GPU results from the previous use of this frame slot
	// Total timers are always active; individual timers only when profiling is enabled
	if (m_timestamps_supported && m_frame_active[frame_index]) {
		uint32_t base = frame_index * QUERIES_PER_FRAME;

		std::array<uint64_t, QUERIES_PER_FRAME * 2> raw{};
		vk::Result result = (*m_device.getDevice()).getQueryPoolResults(
			*m_query_pool, base, QUERIES_PER_FRAME,
			raw.size() * sizeof(uint64_t), raw.data(),
			sizeof(uint64_t) * 2,
			vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability
		);

		if (result == vk::Result::eSuccess || result == vk::Result::eNotReady) {
			for (uint32_t t = 0; t < TIMER_COUNT; t++) {
				if (!m_gpu_active[frame_index][t]) {
					m_results.gpu_ms[t] = 0.0f;
					continue;
				}
				uint32_t start_idx = t * 2;
				uint32_t end_idx = start_idx + 1;
				uint64_t start_ts = raw[start_idx * 2];
				uint64_t start_avail = raw[start_idx * 2 + 1];
				uint64_t end_ts = raw[end_idx * 2];
				uint64_t end_avail = raw[end_idx * 2 + 1];

				if (start_avail && end_avail)
					m_results.gpu_ms[t] = static_cast<float>(end_ts - start_ts) * m_ticks_to_ms;
				else
					m_results.gpu_ms[t] = 0.0f;
			}

		} else {
			m_results.gpu_ms.fill(0.0f);
		}
	}

	m_frame_active[frame_index] = true;
	m_gpu_active[frame_index].fill(false);
	m_current_frame = frame_index;
}

void FrameProfiler::beginGpuTimer(vk::raii::CommandBuffer& cmd, ProfileTimer timer) {
	if (!m_timestamps_supported)
		return;
	if (!m_gpu_enabled && !isTotalTimer(timer))
		return;
	uint32_t idx = queryIndex(m_current_frame, timer, false);
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, *m_query_pool, idx);
	m_gpu_active[m_current_frame][static_cast<uint32_t>(timer)] = true;
}

void FrameProfiler::endGpuTimer(vk::raii::CommandBuffer& cmd, ProfileTimer timer) {
	if (!m_timestamps_supported)
		return;
	if (!m_gpu_enabled && !isTotalTimer(timer))
		return;
	uint32_t idx = queryIndex(m_current_frame, timer, true);
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *m_query_pool, idx);
}

void FrameProfiler::beginCpuTimer(ProfileTimer timer) {
	m_cpu_starts[static_cast<size_t>(timer)] = clock::now();
}

void FrameProfiler::endCpuTimer(ProfileTimer timer) {
	auto end = clock::now();
	auto start = m_cpu_starts[static_cast<size_t>(timer)];
	m_results.cpu_ms[static_cast<size_t>(timer)] +=
		std::chrono::duration<float, std::chrono::milliseconds::period>(end - start).count();
}

void FrameProfiler::resetAllQueries(vk::raii::CommandBuffer& graphics_cmd,
									 vk::raii::CommandBuffer& compute_cmd,
									 uint32_t frame_index) {
	if (!m_timestamps_supported)
		return;
	static constexpr uint32_t GRAPHICS_TIMER_COUNT = static_cast<uint32_t>(ProfileTimer::COMPUTE_TOTAL);
	static constexpr uint32_t COMPUTE_TIMER_COUNT = TIMER_COUNT - GRAPHICS_TIMER_COUNT;

	uint32_t gfx_first = queryIndex(frame_index, ProfileTimer::FRAME_TOTAL, false);
	graphics_cmd.resetQueryPool(*m_query_pool, gfx_first, GRAPHICS_TIMER_COUNT * 2);

	uint32_t comp_first = queryIndex(frame_index, ProfileTimer::COMPUTE_TOTAL, false);
	compute_cmd.resetQueryPool(*m_query_pool, comp_first, COMPUTE_TIMER_COUNT * 2);
}

} // namespace ve
