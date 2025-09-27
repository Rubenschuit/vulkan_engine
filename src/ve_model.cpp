#include "ve_model.hpp"

namespace ve {

	VeModel::VeModel(VeDevice& device, const std::vector<Vertex>& vertices) : ve_device(device) {
		createVertexBuffers(vertices);
	}

	VeModel::VeModel(VeDevice& device, const std::vector<Vertex>& vertices, const std::vector<uint16_t>& indices) : ve_device(device) {
		createVertexBuffers(vertices);
		createIndexBuffers(indices);
	}

	VeModel::~VeModel() {}

	void VeModel::createVertexBuffers(const std::vector<Vertex>& vertices) {
		vertex_count = static_cast<uint32_t>(vertices.size());
		vk::DeviceSize buffer_size = sizeof(vertices[0]) * vertex_count;
		assert(vertex_count >= 3 && "Vertex count must be at least 3!");
		assert(buffer_size >= 1 && "Vertex buffer size must be at least 1 byte!");

		// Create a local scope staging buffer, accessible by CPU
		vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
		vk::MemoryPropertyFlags req_properties = vk::MemoryPropertyFlagBits::eHostVisible |
												 vk::MemoryPropertyFlagBits::eHostCoherent;
		vk::raii::Buffer staging_buffer{nullptr};
		vk::raii::DeviceMemory staging_buffer_memory{nullptr};
		ve_device.createBuffer(buffer_size, usage, req_properties, staging_buffer, staging_buffer_memory);

		// Copy vertex data to staging buffer
		void *data = staging_buffer_memory.mapMemory(0, buffer_size);
		memcpy(data, vertices.data(), buffer_size);
		staging_buffer_memory.unmapMemory();

		// Create vertex buffer, accessible by GPU only
		usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
		req_properties = vk::MemoryPropertyFlagBits::eDeviceLocal;
		ve_device.createBuffer(buffer_size, usage, req_properties, vertex_buffer, vertex_buffer_memory);

		// Copy vertex data from staging buffer to vertex buffer
		ve_device.copyBuffer(staging_buffer, vertex_buffer, buffer_size);
	}

	void VeModel::createIndexBuffers(const std::vector<uint16_t>& indices) {
		index_count = static_cast<uint32_t>(indices.size());
		vk::DeviceSize buffer_size = sizeof(indices[0]) * index_count;
		assert(index_count >= 3 && "Index count must be at least 3!");
		assert(buffer_size >= 1 && "Index buffer size must be at least 1 byte!");

		// Create a local scope staging buffer, accessible by CPU
		vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
		vk::MemoryPropertyFlags req_properties = vk::MemoryPropertyFlagBits::eHostVisible |
												 vk::MemoryPropertyFlagBits::eHostCoherent;
		vk::raii::Buffer staging_buffer{nullptr};
		vk::raii::DeviceMemory staging_buffer_memory{nullptr};
		ve_device.createBuffer(buffer_size, usage, req_properties, staging_buffer, staging_buffer_memory);

		// Copy index data to staging buffer
		void *data = staging_buffer_memory.mapMemory(0, buffer_size);
		memcpy(data, indices.data(), buffer_size);
		staging_buffer_memory.unmapMemory();

		// Create index buffer, accessible by GPU only
		usage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
		req_properties = vk::MemoryPropertyFlagBits::eDeviceLocal;
		ve_device.createBuffer(buffer_size, usage, req_properties, index_buffer, index_buffer_memory);

		// Copy index data from staging buffer to index buffer
		ve_device.copyBuffer(staging_buffer, index_buffer, buffer_size);
	}

	void VeModel::bindVertexBuffer(vk::CommandBuffer command_buffer) {
		vk::Buffer buffers[] = { *vertex_buffer };
		vk::DeviceSize offsets[] = { 0 };
		command_buffer.bindVertexBuffers(0, 1, buffers, offsets);
	}

	void VeModel::bindIndexBuffer(vk::CommandBuffer command_buffer) {
		command_buffer.bindIndexBuffer(*index_buffer, 0, vk::IndexType::eUint16);
	}

	void VeModel::draw(vk::CommandBuffer command_buffer) {
		command_buffer.draw(vertex_count, 1, 0, 0);
	}

	void VeModel::drawIndexed(vk::CommandBuffer command_buffer) {
		command_buffer.drawIndexed(index_count, 1, 0, 0, 0);
	}

	std::vector<vk::VertexInputBindingDescription> VeModel::Vertex::getBindingDescriptions() {
		std::vector<vk::VertexInputBindingDescription> binding_descriptions(1);
		binding_descriptions[0] = vk::VertexInputBindingDescription{
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = vk::VertexInputRate::eVertex
		};
		return binding_descriptions;
	}

	std::vector<vk::VertexInputAttributeDescription> VeModel::Vertex::getAttributeDescriptions() {
		std::vector<vk::VertexInputAttributeDescription> attribute_descriptions(2);
		attribute_descriptions[0] = vk::VertexInputAttributeDescription{
			.location = 0,
			.binding = 0,
			.format = vk::Format::eR32G32Sfloat,
			.offset = offsetof(Vertex, pos),
		};
		attribute_descriptions[1] = vk::VertexInputAttributeDescription{
			.location = 1,
			.binding = 0,
			.format = vk::Format::eR32G32B32Sfloat,
			.offset = offsetof(Vertex, color)
		};
		return attribute_descriptions;
	}
}