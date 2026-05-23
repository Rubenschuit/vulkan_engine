/*
 * Spawns N-1 std::thread workers scoped to the call, reuses the calling
 * thread as worker 0. Uses an atomic counter for dynamic work distribution
 *
 * Not a long-lived thread pool, workers exit when the index range is exhausted
 * or `cancelled` is set, and are joined before returning
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace ve::parallel {

// Runs fn(i) for i in [0, count). Workers pull indices from a shared atomic counter.
// Each worker checks `cancelled` between items and exits early if set.
// max_workers == 0 means std::thread::hardware_concurrency() - 1
inline void forIndexed(size_t count, std::atomic<bool>& cancelled,
                       const std::function<void(size_t)>& fn,
                       uint32_t max_workers = 0) {
	if (count == 0)
		return;

	if (max_workers == 0) {
		uint32_t hw = std::thread::hardware_concurrency();
		max_workers = (hw > 1) ? (hw - 1) : 1;
	}
	uint32_t worker_count = static_cast<uint32_t>(std::min<size_t>(max_workers, count));
	if (worker_count == 0)
		worker_count = 1;

	std::atomic<size_t> next_index{0};

	auto worker = [&]() {
		while (true) {
			if (cancelled.load(std::memory_order_relaxed))
				return;
			size_t i = next_index.fetch_add(1, std::memory_order_relaxed);
			if (i >= count)
				return;
			fn(i);
		}
	};

	std::vector<std::thread> threads;
	threads.reserve(worker_count - 1);
	for (uint32_t w = 1; w < worker_count; w++)
		threads.emplace_back(worker);
	worker();
	for (auto& t : threads)
		t.join();
}

} 