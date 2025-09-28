#include "ve_buffer.hpp"
#include <cassert>
#include <iostream>

namespace ve {
	// Aligns instance size to the minimum offset alignment required by the device,
	// a uniform buffer with instance size of 19 bytes gets padded to become 32 bytes
	// if minOffsetAlignment is 16 bytes
	vk::DeviceSize VeBuffer::getAlignment(vk::DeviceSize instance_size, vk::DeviceSize min_offset_alignment) {
		if (min_offset_alignment > 0) {
			// Vulkan guarantees minUniformBufferOffsetAlignment and minStorageBufferOffsetAlignment
			// are powers of two. Enforce in debug builds to catch misuse.
			assert((min_offset_alignment & (min_offset_alignment - 1)) == 0 && "min_offset_alignment must be power-of-two");
			// Here we round up to the nearest multiple of minOffsetAlignment
			return (instance_size + min_offset_alignment - 1) & ~(min_offset_alignment - 1);
		}
		return instance_size;
	}

	VeBuffer::VeBuffer(
			VeDevice& ve_device,
			vk::DeviceSize instance_size,
			uint32_t instance_count,
			vk::BufferUsageFlags usage_flags,
			vk::MemoryPropertyFlags memory_property_flags,
			vk::DeviceSize min_offset_alignment)
		: ve_device(ve_device),
		instance_size(instance_size),
		instance_count(instance_count),
		usage_flags(usage_flags),
		memory_property_flags(memory_property_flags) {

		alignment_size = getAlignment(instance_size, min_offset_alignment);
		buffer_size = alignment_size * instance_count;

		ve_device.createBuffer(
			buffer_size,
			usage_flags,
			memory_property_flags,
			buffer,
			buffer_memory);
	}

	VeBuffer::~VeBuffer() {
		unmap();
		// buffer and buffer_memory are RAII objects and will be cleaned up automatically
	}

	void VeBuffer::map(vk::DeviceSize size, vk::DeviceSize offset) {
		assert(buffer_memory != nullptr && "Buffer memory is null");
		mapped = buffer_memory.mapMemory(offset, size);
		assert(mapped != nullptr && "Failed to map buffer memory");
	}

	void VeBuffer::unmap() {
		if (mapped) {
			buffer_memory.unmapMemory();
			mapped = nullptr;
		}
	}

	void VeBuffer::writeToBuffer(void* data, vk::DeviceSize size, vk::DeviceSize offset) {
		assert(mapped != nullptr && "Cannot write to unmapped buffer");
		assert(size > buffer_size && "Size exceeds buffer size");
		assert(offset + size > buffer_size && "Write exceeds buffer size");
		// If size is VK_WHOLE_SIZE, we write the whole buffer
		if (size == VK_WHOLE_SIZE) {
			memcpy(mapped, data, buffer_size);
		}
		else {
			char* mem_offset = (char*)mapped;
			mem_offset += offset;
			memcpy(mem_offset, data, size);
		}
	}

}