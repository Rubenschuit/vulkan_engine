#pragma once

#include "ve_device.hpp"

namespace ve {
	class VeBuffer {
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

			void map(vk::DeviceSize size = VK_WHOLE_SIZE, vk::DeviceSize offset = 0);
			void unmap();

			void writeToBuffer(void* data, vk::DeviceSize size = VK_WHOLE_SIZE, vk::DeviceSize offset = 0);


			vk::raii::Buffer& getBuffer() { return buffer; }
			void* getMappedMemory() const { return mapped; }
			uint32_t getInstanceCount() const { return instance_count; }
			vk::DeviceSize getInstanceSize() const { return instance_size; }
			vk::DeviceSize getAlignmentSize() const { return alignment_size; }
			vk::DeviceSize getBufferSize() const { return buffer_size; }

			private:
			VeDevice& ve_device;
			void* mapped = nullptr;
			vk::raii::Buffer buffer{nullptr};
			vk::raii::DeviceMemory buffer_memory{nullptr};

			vk::DeviceSize instance_size;
			uint32_t instance_count;
			vk::DeviceSize alignment_size;
			vk::DeviceSize buffer_size;
			vk::BufferUsageFlags usage_flags;
			vk::MemoryPropertyFlags memory_property_flags;

	};
}