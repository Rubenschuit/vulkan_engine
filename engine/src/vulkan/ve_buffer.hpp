#pragma once

#include "ve_export.hpp"
#include "vulkan/ve_device.hpp"

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace ve {

class VENGINE_API VeBuffer {
public:
	static vk::DeviceSize getAlignment(vk::DeviceSize instance_size, vk::DeviceSize min_offset_alignment);
	VeBuffer(
		VeDevice& ve_device,
		vk::DeviceSize instance_size,
		uint32_t instance_count,
		vk::BufferUsageFlags usage_flags,
		vk::MemoryPropertyFlags memory_property_flags,
		vk::DeviceSize min_offset_alignment = 1);
	~VeBuffer();

	VeBuffer(const VeBuffer&) = delete;
	VeBuffer& operator=(const VeBuffer&) = delete;

	// getters
	vk::Buffer getBuffer() const { return m_buffer; }
	void* getMappedMemory() const { return m_mapped; }
	uint32_t getInstanceCount() const { return m_instance_count; }
	vk::DeviceSize getInstanceSize() const { return m_instance_size; }
	vk::DeviceSize getAlignmentSize() const { return m_alignment_size; }
	vk::DeviceSize getBufferSize() const { return m_buffer_size; }
	vk::DescriptorBufferInfo getDescriptorInfo(vk::DeviceSize size = VK_WHOLE_SIZE, vk::DeviceSize offset = 0) const;

	void map(vk::DeviceSize size = VK_WHOLE_SIZE, vk::DeviceSize offset = 0);
	void unmap();
	void writeToBuffer(const void* data, vk::DeviceSize size = VK_WHOLE_SIZE, vk::DeviceSize offset = 0);

private:
	VeDevice& m_ve_device;
	void* m_mapped = nullptr;
	vk::Buffer m_buffer{};
	VmaAllocation m_allocation = nullptr;

	vk::DeviceSize m_instance_size;
	uint32_t m_instance_count;
	vk::DeviceSize m_alignment_size;
	vk::DeviceSize m_buffer_size;
	vk::BufferUsageFlags m_usage_flags;
	vk::MemoryPropertyFlags m_memory_property_flags;

};
}
