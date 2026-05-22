#include "pch.hpp"
#include "resources/internal/staging_arena.hpp"
#include "vulkan/ve_device.hpp"

#include <cstring>

namespace ve {

// Aligned to satisfy buffer-to-image bufferOffset requirements for any format.
// Also respect the device's optimalBufferCopyOffsetAlignment hint.
static vk::DeviceSize pickAlignment(VeDevice& device) {
	vk::DeviceSize hint = device.getDeviceProperties().limits.optimalBufferCopyOffsetAlignment;
	return std::max<vk::DeviceSize>(16, hint);
}

StagingArena::StagingArena(VeDevice& device, vk::DeviceSize capacity)
	: m_device(device),
	  m_buffer(device, capacity, 1,
	           vk::BufferUsageFlagBits::eTransferSrc,
	           vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent),
	  m_capacity(capacity),
	  m_alignment(pickAlignment(device)) {
	m_buffer.map();
	m_buffer.setDebugName("StagingArena");
}

StagingArena::Alloc StagingArena::reserve(vk::DeviceSize size, void** out_ptr) {
	vk::DeviceSize start = align(m_offset);
	if (start + size <= m_capacity) {
		if (out_ptr)
			*out_ptr = static_cast<uint8_t*>(m_buffer.getMappedMemory()) + start;
		m_offset = start + size;
		return {m_buffer.getBuffer(), start};
	}

	// Spill path: allocate a one-off staging buffer sized for this request.
	auto spill = std::make_unique<VeBuffer>(
		m_device, size, 1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	spill->map();
	if (out_ptr)
		*out_ptr = spill->getMappedMemory();
	vk::Buffer buf = spill->getBuffer();
	m_spill.push_back(std::move(spill));
	return {buf, 0};
}

StagingArena::Alloc StagingArena::write(const void* src, vk::DeviceSize size) {
	void* dst = nullptr;
	Alloc a = reserve(size, &dst);
	std::memcpy(dst, src, size);
	return a;
}

void StagingArena::reset() {
	m_offset = 0;
	m_spill.clear();
}

}