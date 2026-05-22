#include "pch.hpp"
#include "vulkan/ve_buffer.hpp"
#include <vk_mem_alloc.h>
#include <cassert>

namespace ve {

vk::DeviceSize VeBuffer::getAlignment(vk::DeviceSize instance_size, vk::DeviceSize min_offset_alignment) {
	if (min_offset_alignment > 0) {
		assert((min_offset_alignment & (min_offset_alignment - 1)) == 0 && "min_offset_alignment must be power-of-two");
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
	: m_ve_device(ve_device),
		m_instance_size(instance_size),
		m_instance_count(instance_count),
		m_usage_flags(usage_flags),
		m_memory_property_flags(memory_property_flags) {

	m_alignment_size = getAlignment(instance_size, min_offset_alignment);
	m_buffer_size = m_alignment_size * instance_count;

	auto unique_families = ve_device.getQueueFamilyIndices().uniqueFamilies();
	bool use_concurrent = !ve_device.getQueueFamilyIndices().allSameFamily() && unique_families.size() > 1;

	VkBufferCreateInfo buffer_info {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = m_buffer_size,
		.usage = static_cast<VkBufferUsageFlags>(m_usage_flags),
		.sharingMode = use_concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = use_concurrent ? static_cast<uint32_t>(unique_families.size()) : 0u,
		.pQueueFamilyIndices = use_concurrent ? unique_families.data() : nullptr,
	};

	VmaAllocationCreateInfo alloc_info{};
	alloc_info.requiredFlags = static_cast<VkMemoryPropertyFlags>(memory_property_flags);

	if (memory_property_flags & vk::MemoryPropertyFlagBits::eHostVisible) {
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
		                 | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	} else {
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
	}

	VkBuffer vk_buffer;
	VmaAllocationInfo vma_alloc_info{};
	if (vmaCreateBuffer(ve_device.getAllocator(), &buffer_info, &alloc_info,
	                    &vk_buffer, &m_allocation, &vma_alloc_info) != VK_SUCCESS)
		throw std::runtime_error("VMA: failed to create buffer");

	m_buffer = vk::Buffer(vk_buffer);

	if (memory_property_flags & vk::MemoryPropertyFlagBits::eHostVisible)
		m_mapped = vma_alloc_info.pMappedData;
}

VeBuffer::~VeBuffer() {
	unmap();
	if (m_allocation)
		vmaDestroyBuffer(m_ve_device.getAllocator(), static_cast<VkBuffer>(m_buffer), m_allocation);
}

void VeBuffer::map(vk::DeviceSize size, vk::DeviceSize offset) {
	(void)size;
	(void)offset;
	if (!m_mapped) {
		VmaAllocationInfo info{};
		vmaGetAllocationInfo(m_ve_device.getAllocator(), m_allocation, &info);
		assert(info.pMappedData && "Tried to map a non-host-visible buffer");
		m_mapped = info.pMappedData;
	}
}

void VeBuffer::unmap() {
	m_mapped = nullptr;
}

void VeBuffer::writeToBuffer(const void* data, vk::DeviceSize size, vk::DeviceSize offset) {
	assert(m_mapped != VK_NULL_HANDLE && "Cannot write to unmapped buffer");
	vk::DeviceSize effective_size = (size == VK_WHOLE_SIZE) ? m_buffer_size : size;
	assert(effective_size <= m_buffer_size && "Size exceeds buffer size");
	assert(offset + effective_size <= m_buffer_size && "Write exceeds buffer size");
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
