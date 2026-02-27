#include "pch.hpp"
#include "vulkan/ve_thread_pool.hpp"

namespace ve {

VeThreadPool::VeThreadPool(CommandResourceManager& cmd_manager, uint32_t num_workers)
	: m_cmd_manager(cmd_manager) {

	m_slots.resize(num_workers);
	m_workers.reserve(num_workers);

	for (uint32_t i = 0; i < num_workers; ++i) {
		m_slots[i] = m_cmd_manager.registerThread();
		m_workers.emplace_back([this, i] { workerLoop(i); });
	}

	VE_LOGI("VeThreadPool: created " + std::to_string(num_workers) + " worker threads");
}

VeThreadPool::~VeThreadPool() {
	m_shutdown.store(true, std::memory_order_release);
	m_dispatch_cv.notify_all();
	for (auto& w : m_workers)
		if (w.joinable())
			w.join();
}

ThreadSlot VeThreadPool::getSlot(uint32_t worker_index) const {
	assert(worker_index < m_slots.size());
	return m_slots[worker_index];
}

void VeThreadPool::resetFrame(uint32_t frame_index) {
	for (uint32_t i = 0; i < static_cast<uint32_t>(m_slots.size()); ++i)
		m_cmd_manager.resetThreadFrame(m_slots[i], frame_index);
}

void VeThreadPool::dispatch(std::function<void(uint32_t, ThreadSlot)> task) {
	uint32_t count = workerCount();
	if (count == 0)
		return;

	{
		std::lock_guard<std::mutex> lock(m_dispatch_mutex);
		m_current_task = std::move(task);
		m_done_count.store(0, std::memory_order_release);
		m_dispatch_epoch.fetch_add(1, std::memory_order_release);
	}
	m_dispatch_cv.notify_all();

	// Wait for all workers to finish
	std::unique_lock<std::mutex> lock(m_dispatch_mutex);
	m_done_cv.wait(lock, [&] {
		return m_done_count.load(std::memory_order_acquire) >= count;
	});
}

void VeThreadPool::workerLoop(uint32_t worker_index) {
	uint64_t last_epoch = 0;

	while (!m_shutdown.load(std::memory_order_acquire)) {
		// Wait for a new dispatch epoch
		{
			std::unique_lock<std::mutex> lock(m_dispatch_mutex);
			m_dispatch_cv.wait(lock, [&] {
				return m_shutdown.load(std::memory_order_acquire)
					|| m_dispatch_epoch.load(std::memory_order_acquire) != last_epoch;
			});
		}

		if (m_shutdown.load(std::memory_order_acquire))
			break;

		last_epoch = m_dispatch_epoch.load(std::memory_order_acquire);

		// Execute the task
		m_current_task(worker_index, m_slots[worker_index]);

		// Signal completion
		if (m_done_count.fetch_add(1, std::memory_order_acq_rel) + 1 >= workerCount())
			m_done_cv.notify_one();
	}
}

} // namespace ve