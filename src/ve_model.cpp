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
		assert(vertex_count >= 3 && "Vertex count must be at least 3!");

		// Create a local scope staging buffer, accessible by CPU
		ve::VeBuffer staging_buffer(
			ve_device,
			sizeof(vertices[0]),
			vertex_count,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		);

		// Copy vertex data to staging buffer
		staging_buffer.map();
		staging_buffer.writeToBuffer((void*)vertices.data());
		// unmap is called in the destructor of VeBuffer

		// Create vertex buffer, accessible by GPU only
		vertex_buffer = std::make_unique<ve::VeBuffer>(
			ve_device,
			sizeof(vertices[0]),
			vertex_count,
			vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			1
		);

		// Copy vertex data from staging buffer to vertex buffer
		auto buffer_size = sizeof(vertices[0]) * vertex_count;
		ve_device.copyBuffer(staging_buffer.getBuffer(), vertex_buffer->getBuffer(), buffer_size);
	}

	void VeModel::createIndexBuffers(const std::vector<uint16_t>& indices) {
		index_count = static_cast<uint32_t>(indices.size());
		assert(index_count >= 3 && "Index count must be at least 3!");

		// Create a local scope staging buffer, accessible by CPU
		ve::VeBuffer staging_buffer(
			ve_device,
			sizeof(indices[0]),
			index_count,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		);

		// Copy index data to staging buffer
		staging_buffer.map();
		staging_buffer.writeToBuffer((void*)indices.data());
		// unmap is called in the destructor of VeBuffer

		// Create index buffer, accessible by GPU only
		index_buffer = std::make_unique<ve::VeBuffer>(
			ve_device,
			sizeof(indices[0]),
			index_count,
			vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			1
		);

		// Copy index data from staging buffer to index buffer
		auto buffer_size = sizeof(indices[0]) * index_count;
		ve_device.copyBuffer(staging_buffer.getBuffer(), index_buffer->getBuffer(), buffer_size);
	}

	void VeModel::bindVertexBuffer(vk::raii::CommandBuffer& command_buffer) {
		vk::Buffer buffers[] = { vertex_buffer->getBuffer() };
		vk::DeviceSize offsets[] = { 0 };
		command_buffer.bindVertexBuffers(0, buffers, offsets);
	}

	void VeModel::bindIndexBuffer(vk::raii::CommandBuffer& command_buffer) {
		command_buffer.bindIndexBuffer(index_buffer->getBuffer(), 0, vk::IndexType::eUint16);
	}

	void VeModel::draw(vk::raii::CommandBuffer& command_buffer) {
		command_buffer.draw(vertex_count, 1, 0, 0);
	}

	void VeModel::drawIndexed(vk::raii::CommandBuffer& command_buffer) {
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
		std::vector<vk::VertexInputAttributeDescription> attribute_descriptions(3);
		attribute_descriptions[0] = vk::VertexInputAttributeDescription{
			.location = 0,
			.binding = 0,
			.format = vk::Format::eR32G32B32Sfloat,
			.offset = offsetof(Vertex, pos),
		};
		attribute_descriptions[1] = vk::VertexInputAttributeDescription{
			.location = 1,
			.binding = 0,
			.format = vk::Format::eR32G32B32Sfloat,
			.offset = offsetof(Vertex, color)
		};
		attribute_descriptions[2] = vk::VertexInputAttributeDescription{
			.location = 2,
			.binding = 0,
			.format = vk::Format::eR32G32Sfloat,
			.offset = offsetof(Vertex, tex_coord)
		};
		return attribute_descriptions;
	}

} // namespace ve