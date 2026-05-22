/* StagingArena - host-visible buffer for batched GPU uploads.
 * One VMA allocation per arena
 *
 * Contract: caller keeps the arena alive until the GPU has consumed
 * every recorded copy referencing it
 *
 * Oversized writes (> remaining capacity) spill to a one-off VeBuffer the
 * arena owns. Spills are released on reset()
 */
#pragma once
#include "ve_export.hpp"
#include "vulkan/ve_buffer.hpp"

#include <memory>
#include <vector>

namespace ve {

class VENGINE_API StagingArena {
public:
	struct Alloc {
		vk::Buffer buffer{};
		vk::DeviceSize offset{0};
	};

	// `capacity` is the size of the main buffer. Requests that
	// don't fit spill to per-request buffers.
	StagingArena(VeDevice& device, vk::DeviceSize capacity);

	StagingArena(const StagingArena&) = delete;
	StagingArena& operator=(const StagingArena&) = delete;

	// Reserve `size` bytes (aligned) and copy `src` into them.
	Alloc write(const void* src, vk::DeviceSize size);

	// Reserve `size` bytes (aligned); fill the returned pointer yourself.
	Alloc reserve(vk::DeviceSize size, void** out_ptr);

	// Caller guarantees the GPU is done with prior work referencing this arena.
	void reset();

	vk::DeviceSize capacity() const { return m_capacity; }
	vk::DeviceSize used() const { return m_offset; }
	size_t spillCount() const { return m_spill.size(); }

private:
	vk::DeviceSize align(vk::DeviceSize x) const {
		return (x + m_alignment - 1) & ~(m_alignment - 1);
	}

	VeDevice& m_device;
	VeBuffer m_buffer;
	vk::DeviceSize m_capacity;
	vk::DeviceSize m_offset = 0;
	vk::DeviceSize m_alignment;
	std::vector<std::unique_ptr<VeBuffer>> m_spill;
};

}