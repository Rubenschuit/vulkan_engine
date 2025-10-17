#include "pch.hpp"
#include "ve_buffer.hpp"
#include <cassert>

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

	VeBuffer::VeBuffer(VeDevice& ve_device,
					   vk::DeviceSize instance_size,
					   uint32_t instance_count,
					   vk::BufferUsageFlags usage_flags,
					   vk::MemoryPropertyFlags memory_property_flags,
					   vk::DeviceSize min_offset_alignment)
		: m_instance_size(instance_size),
		  m_instance_count(instance_count),
		  m_usage_flags(usage_flags),
		  m_memory_property_flags(memory_property_flags) {

		m_alignment_size = getAlignment(instance_size, min_offset_alignment);
		m_buffer_size = m_alignment_size * instance_count;

		ve_device.createBuffer(
			m_buffer_size,
			m_usage_flags,
			m_memory_property_flags,
			m_buffer,
			m_buffer_memory);
	}

	VeBuffer::~VeBuffer() {
		unmap();
		// buffer and buffer_memory are RAII objects and will be cleaned up automatically
	}

	void VeBuffer::map(vk::DeviceSize size, vk::DeviceSize offset) {
		assert(m_buffer_memory != VK_NULL_HANDLE && "Buffer memory is null");
		m_mapped = m_buffer_memory.mapMemory(offset, size);
		assert(m_mapped != VK_NULL_HANDLE && "Failed to map buffer memory");
	}

	void VeBuffer::unmap() {
		if (m_mapped) {
			m_buffer_memory.unmapMemory();
			m_mapped = nullptr;
		}
	}

	void VeBuffer::writeToBuffer(void* data, vk::DeviceSize size, vk::DeviceSize offset) {
		assert(m_mapped != VK_NULL_HANDLE && "Cannot write to unmapped buffer");
		vk::DeviceSize effective_size = (size == VK_WHOLE_SIZE) ? m_buffer_size : size;
		assert(effective_size <= m_buffer_size && "Size exceeds buffer size");
		assert(offset + effective_size <= m_buffer_size && "Write exceeds buffer size");
		// If size is VK_WHOLE_SIZE, we write the whole buffer
		if (size == VK_WHOLE_SIZE) {
			memcpy(m_mapped, data, m_buffer_size);
		} else {
			char* mem_offset = static_cast<char*>(m_mapped);
			mem_offset += offset;
			memcpy(mem_offset, data, effective_size);
		}
	}

	vk::DescriptorBufferInfo VeBuffer::getDescriptorInfo(vk::DeviceSize size, vk::DeviceSize offset) const {
		vk::DescriptorBufferInfo buffer_info{
			.buffer = m_buffer,
			.offset = offset,
			.range = size
		};
		return buffer_info;
	}
}