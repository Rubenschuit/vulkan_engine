/* Worker thread pool for parallel command buffer recording.
 * Each worker owns a ThreadSlot with per-frame command pools, registered
 * with CommandResourceManager at construction.  
 * */
#pragma once
#include "ve_export.hpp"
#include "ve_command_resource_manager.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ve {

class VENGINE_API VeThreadPool {
public:
	VeThreadPool(CommandResourceManager& cmd_manager, uint32_t num_workers);
	~VeThreadPool();

	VeThreadPool(const VeThreadPool&) = delete;
	VeThreadPool& operator=(const VeThreadPool&) = delete;

	uint32_t workerCount() const { return static_cast<uint32_t>(m_workers.size()); }
	ThreadSlot getSlot(uint32_t worker_index) const;

	/// Reset all workers' command pools for the given frame.
	void resetFrame(uint32_t frame_index);

	/// Dispatch task(worker_index, slot) to each worker. Blocks until all complete.
	void dispatch(std::function<void(uint32_t, ThreadSlot)> task);

private:
	void workerLoop(uint32_t worker_index);

	CommandResourceManager& m_cmd_manager;
	std::vector<std::jthread> m_workers;
	std::vector<ThreadSlot> m_slots;

	// Epoch-based dispatch synchronization
	std::function<void(uint32_t, ThreadSlot)> m_current_task;
	std::mutex m_dispatch_mutex;
	std::condition_variable m_dispatch_cv;
	std::condition_variable m_done_cv;
	std::atomic<uint64_t> m_dispatch_epoch{0};
	std::atomic<uint32_t> m_done_count{0};
	std::atomic<bool> m_shutdown{false};
};

} // namespace ve