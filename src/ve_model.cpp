#include "ve_model.hpp"

namespace ve {

	VeModel::VeModel(VeDevice& device, const std::vector<Vertex>& vertices) : ve_device(device) {
		createVertexBuffers(vertices);
	}

	VeModel::~VeModel() {}

	void VeModel::createVertexBuffers(const std::vector<Vertex>& vertices) {
		vertex_count = static_cast<uint32_t>(vertices.size());
		assert(vertex_count > 2 && "Vertex buffer requires at least 3 vertices");

		vk::DeviceSize buffer_size = sizeof(vertices[0]) * vertex_count;
		vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eVertexBuffer;
		vk::MemoryPropertyFlags req_properties = vk::MemoryPropertyFlagBits::eHostVisible |
												 vk::MemoryPropertyFlagBits::eHostCoherent;

		ve_device.createBuffer(buffer_size, usage, req_properties, vertex_buffer, vertex_buffer_memory);

		void *data = vertex_buffer_memory.mapMemory(0, buffer_size);
		memcpy(data, vertices.data(), buffer_size);
		vertex_buffer_memory.unmapMemory();
	}

	void VeModel::bind(vk::CommandBuffer command_buffer) {
		vk::Buffer buffers[] = { *vertex_buffer };
		vk::DeviceSize offsets[] = { 0 };
		command_buffer.bindVertexBuffers(0, 1, buffers, offsets);
	}

	void VeModel::draw(vk::CommandBuffer command_buffer) {
		command_buffer.draw(vertex_count, 1, 0, 0);
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