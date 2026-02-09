#include "pch.hpp"
#include "game/ve_mesh.hpp"

namespace ve {

VeMesh::VeMesh(VeDevice& device, const std::string& resource_id,
               const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
	: Resource(resource_id), m_ve_device(device) {
	createVertexBuffers(vertices);
	createIndexBuffers(indices);
	setLoaded(true);
}

VeMesh::~VeMesh() {
	unload();
}

bool VeMesh::doLoad() {
	// Meshes are created from data, not loaded from file
	return true;
}

void VeMesh::doUnload() {
	m_vertex_buffer.reset();
	m_index_buffer.reset();
	m_vertex_count = 0;
	m_index_count = 0;
}

void VeMesh::createVertexBuffers(const std::vector<Vertex>& vertices) {
	m_vertex_count = static_cast<uint32_t>(vertices.size());
	assert(m_vertex_count >= 3 && "Vertex count must be at least 3");

	VeBuffer staging_buffer(
		m_ve_device,
		sizeof(vertices[0]),
		m_vertex_count,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(const_cast<void*>(static_cast<const void*>(vertices.data())));

	m_vertex_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(vertices[0]),
		m_vertex_count,
		vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		1
	);
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_vertex_buffer->getBuffer(),
	                       sizeof(vertices[0]) * m_vertex_count);
}

void VeMesh::createIndexBuffers(const std::vector<uint32_t>& indices) {
	m_index_count = static_cast<uint32_t>(indices.size());
	assert(m_index_count >= 3 && "Index count must be at least 3");

	VeBuffer staging_buffer(
		m_ve_device,
		sizeof(indices[0]),
		m_index_count,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(const_cast<void*>(static_cast<const void*>(indices.data())));

	m_index_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(indices[0]),
		m_index_count,
		vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		1
	);
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_index_buffer->getBuffer(),
	                       sizeof(indices[0]) * m_index_count);
}

void VeMesh::bindVertexBuffer(vk::raii::CommandBuffer& command_buffer) const {
	vk::Buffer buffers[] = {*m_vertex_buffer->getBuffer()};
	vk::DeviceSize offsets[] = {0};
	command_buffer.bindVertexBuffers(0, buffers, offsets);
}

void VeMesh::bindIndexBuffer(vk::raii::CommandBuffer& command_buffer) const {
	command_buffer.bindIndexBuffer(*m_index_buffer->getBuffer(), 0, vk::IndexType::eUint32);
}

void VeMesh::draw(vk::raii::CommandBuffer& command_buffer) const {
	command_buffer.draw(m_vertex_count, 1, 0, 0);
}

void VeMesh::drawIndexed(vk::raii::CommandBuffer& command_buffer) const {
	command_buffer.drawIndexed(m_index_count, 1, 0, 0, 0);
}

std::vector<vk::VertexInputBindingDescription> VeMesh::Vertex::getBindingDescriptions() {
	return {vk::VertexInputBindingDescription{
		.binding = 0,
		.stride = sizeof(Vertex),
		.inputRate = vk::VertexInputRate::eVertex
	}};
}

std::vector<vk::VertexInputAttributeDescription> VeMesh::Vertex::getAttributeDescriptionsSimple() {
	return {
		vk::VertexInputAttributeDescription{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, pos)},
		vk::VertexInputAttributeDescription{.location = 1, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, color)},
		vk::VertexInputAttributeDescription{.location = 2, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, normal)},
		vk::VertexInputAttributeDescription{.location = 3, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(Vertex, tex_coord)}
	};
}

std::vector<vk::VertexInputAttributeDescription> VeMesh::Vertex::getAttributeDescriptions() {
	return {
		vk::VertexInputAttributeDescription{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, pos)},
		vk::VertexInputAttributeDescription{.location = 1, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, color)},
		vk::VertexInputAttributeDescription{.location = 2, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, normal)},
		vk::VertexInputAttributeDescription{.location = 3, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(Vertex, tex_coord)},
		vk::VertexInputAttributeDescription{.location = 4, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(Vertex, tangent)}
	};
}

std::vector<vk::VertexInputAttributeDescription> VeMesh::Vertex::getAttributeDescriptionsShadow() {
	return {vk::VertexInputAttributeDescription{
		.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, pos)
	}};
}

} // namespace ve
