#include "pch.hpp"
#include "ve_model.hpp"

#define TINYOBJLOADER_IMPLEMENTATION // define this in only *one* .cpp file
#include <tiny_obj_loader.h>

namespace ve {



	VeModel::VeModel(VeDevice& device, const std::vector<Vertex>& vertices) : m_ve_device(device) {
		createVertexBuffers(vertices);
	}

	VeModel::VeModel(VeDevice& device, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) : m_ve_device(device) {
		createVertexBuffers(vertices);
		createIndexBuffers(indices);
	}

	VeModel::VeModel(VeDevice& device, char const* model_path) : m_ve_device(device) {
		tinyobj::attrib_t attrib;
		std::vector<tinyobj::shape_t> shapes;
		std::vector<tinyobj::material_t> materials;
		std::string warn, err;

		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, model_path)) {
			throw std::runtime_error(warn + err);
		}
		if (shapes.size() == 0) {
			throw std::runtime_error("Model contains no shapes");
		}

		std::vector<Vertex> vertices;
		std::unordered_map<Vertex, uint32_t> unique_vertices{};
		std::vector<uint32_t> indices;
		for (const auto& shape : shapes) {
			for (const auto& index: shape.mesh.indices) {
				Vertex vertex{};

				vertex.pos = {
					attrib.vertices[static_cast<size_t>(3 * index.vertex_index)],
					attrib.vertices[static_cast<size_t>(3 * index.vertex_index + 1)],
					attrib.vertices[static_cast<size_t>(3 * index.vertex_index + 2)]
				};

				// if no color data is present, default to white
				vertex.color = {
					attrib.colors[static_cast<size_t>(3 * index.vertex_index)],
					attrib.colors[static_cast<size_t>(3 * index.vertex_index + 1)],
					attrib.colors[static_cast<size_t>(3 * index.vertex_index + 2)]
				};

				vertex.normal = {
					attrib.normals[static_cast<size_t>(3 * index.normal_index)],
					attrib.normals[static_cast<size_t>(3 * index.normal_index + 1)],
					attrib.normals[static_cast<size_t>(3 * index.normal_index + 2)]
				};

				vertex.tex_coord = {
					attrib.texcoords[static_cast<size_t>(2 * index.texcoord_index)],
					1.0f - attrib.texcoords[static_cast<size_t>(2 * index.texcoord_index + 1)], // .obj vs vulkan texture coords
				};

				if (unique_vertices.count(vertex) == 0) {
					unique_vertices[vertex] = static_cast<uint32_t>(vertices.size());
					vertices.push_back(vertex);
				}

				indices.push_back(unique_vertices[vertex]);
			}
		}
		VE_LOGI("Model " << model_path << " has " << vertices.size() << " vertices and " << indices.size() << " indices");
		createVertexBuffers(vertices);
		createIndexBuffers(indices);
	}

	VeModel::~VeModel() {}

	void VeModel::createVertexBuffers(const std::vector<Vertex>& vertices) {
		m_vertex_count = static_cast<uint32_t>(vertices.size());
		assert(m_vertex_count >= 3 && "Vertex count must be at least 3!");

		// Create a local scope staging buffer, accessible by CPU
		ve::VeBuffer staging_buffer(
			m_ve_device,
			sizeof(vertices[0]),
			m_vertex_count,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		);

		// Copy vertex data to staging buffer
		staging_buffer.map();
		staging_buffer.writeToBuffer((void*)vertices.data());
		// unmap is called in the destructor of VeBuffer

		// Create vertex buffer, accessible by GPU only
		m_vertex_buffer = std::make_unique<ve::VeBuffer>(
			m_ve_device,
			sizeof(vertices[0]),
			m_vertex_count,
			vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			1
		);

		// Copy vertex data from staging buffer to vertex buffer
		auto buffer_size = sizeof(vertices[0]) * m_vertex_count;
		m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_vertex_buffer->getBuffer(), buffer_size);
	}

	void VeModel::createIndexBuffers(const std::vector<uint32_t>& indices) {
		m_index_count = static_cast<uint32_t>(indices.size());
		assert(m_index_count >= 3 && "Index count must be at least 3!");

		// Create a local scope staging buffer, accessible by CPU
		ve::VeBuffer staging_buffer(
			m_ve_device,
			sizeof(indices[0]),
			m_index_count,
			vk::BufferUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		);

		// Copy index data to staging buffer
		staging_buffer.map();
		staging_buffer.writeToBuffer((void*)indices.data());
		// unmap is called in the destructor of VeBuffer

		// Create index buffer, accessible by GPU only
		m_index_buffer = std::make_unique<ve::VeBuffer>(
			m_ve_device,
			sizeof(indices[0]),
			m_index_count,
			vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			1
		);

		// Copy index data from staging buffer to index buffer
		auto buffer_size = sizeof(indices[0]) * m_index_count;
		m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_index_buffer->getBuffer(), buffer_size);
	}

	void VeModel::bindVertexBuffer(vk::raii::CommandBuffer& command_buffer) {
		vk::Buffer buffers[] = { m_vertex_buffer->getBuffer() };
		vk::DeviceSize offsets[] = { 0 };
		command_buffer.bindVertexBuffers(0, buffers, offsets);
	}

	void VeModel::bindIndexBuffer(vk::raii::CommandBuffer& command_buffer) {
		command_buffer.bindIndexBuffer(m_index_buffer->getBuffer(), 0, vk::IndexType::eUint32);
	}

	void VeModel::draw(vk::raii::CommandBuffer& command_buffer) {
		command_buffer.draw(m_vertex_count, 1, 0, 0);
	}

	void VeModel::drawIndexed(vk::raii::CommandBuffer& command_buffer) {
		command_buffer.drawIndexed(m_index_count, 1, 0, 0, 0);
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
		std::vector<vk::VertexInputAttributeDescription> attribute_descriptions(4);
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
			.format = vk::Format::eR32G32B32Sfloat,
			.offset = offsetof(Vertex, normal)
		};
		attribute_descriptions[3] = vk::VertexInputAttributeDescription{
			.location = 3,
			.binding = 0,
			.format = vk::Format::eR32G32Sfloat,
			.offset = offsetof(Vertex, tex_coord)
		};
		return attribute_descriptions;
	}

} // namespace ve