/* Thread-safe command pool and command buffer manager.
 * Centralises all per-frame command pool/buffer ownership:
 * - Primary CBs (graphics, compute, UI) for the main thread
 * - Per-thread secondary pools with watermark allocation of secondary CBs
 */
#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <array>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ve {

class VeDevice;

/// Opaque slot handle returned by registerThread().
struct ThreadSlot {
	uint32_t id = UINT32_MAX;
	bool valid() const { return id != UINT32_MAX; }
};

class VENGINE_API CommandResourceManager {
public:
	CommandResourceManager(VeDevice& device);
	~CommandResourceManager();

	CommandResourceManager(const CommandResourceManager&) = delete;
	CommandResourceManager& operator=(const CommandResourceManager&) = delete;

	// ── Primary command buffers (main thread only) ─────────────────

	vk::raii::CommandBuffer& getGraphicsPrimary(uint32_t frame_index);
	vk::raii::CommandBuffer& getComputePrimary(uint32_t frame_index);
	vk::raii::CommandBuffer& getUIPrimary(uint32_t frame_index);

	/// Reset all primary CBs for the given frame slot.
	void resetPrimaries(uint32_t frame_index);

	// ── Thread registration (thread-safe) ──────────────────────────

	/// Register a worker thread. Returns a slot with its own per-frame
	/// command pool (graphics family, eTransient).
	ThreadSlot registerThread();

	uint32_t threadSlotCount() const;

	// ── Secondary command buffers (per-thread, per-frame) ──────────

	/// Pool-level reset for a thread slot's frame. Call at frame start
	/// from the owning thread.
	void resetThreadFrame(ThreadSlot slot, uint32_t frame_index);

	/// Acquire a secondary CB from the slot's pool. Watermark pattern:
	/// reuses existing handles when available, allocates new ones as needed.
	/// The returned CB is already in the recording state (begin called).
	vk::raii::CommandBuffer& acquireSecondary(ThreadSlot slot, uint32_t frame_index);

	uint32_t secondaryCount(ThreadSlot slot, uint32_t frame_index) const;

	// ── Pool accessors ─────────────────────────────────────────────

	vk::raii::CommandPool& getGraphicsPool() { return m_graphics_pool; }
	vk::raii::CommandPool& getComputePool() { return m_compute_pool; }

private:
	void createPrimaryResources();

	VeDevice& m_device;

	// Primary pools (eResetCommandBuffer) + primary CBs [frame_index]
	vk::raii::CommandPool m_graphics_pool{nullptr};
	vk::raii::CommandPool m_compute_pool{nullptr};
	vk::raii::CommandPool m_ui_pool{nullptr};
	std::vector<vk::raii::CommandBuffer> m_graphics_primaries;
	std::vector<vk::raii::CommandBuffer> m_compute_primaries;
	std::vector<vk::raii::CommandBuffer> m_ui_primaries;

	// Per-thread secondary state
	struct PerFrameState {
		vk::raii::CommandPool pool{nullptr};                // eTransient, graphics family
		std::vector<vk::raii::CommandBuffer> buffers;       // watermark array
		uint32_t active_count = 0;
	};

	struct SlotData {
		std::array<PerFrameState, MAX_FRAMES_IN_FLIGHT> frames;
	};

	std::vector<SlotData> m_slots;
	mutable std::mutex m_registration_mutex; // only protects registerThread()
};

} // namespace ve