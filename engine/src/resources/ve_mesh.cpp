#include "pch.hpp"
#include "resources/ve_mesh.hpp"
#include "rendering/meshlet_data.hpp"
#include "ve_config.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <meshoptimizer.h>

namespace ve {

VeMesh::AABB transformAABB(const VeMesh::AABB& local, const glm::mat4& model) {
	const glm::vec3& mn = local.min;
	const glm::vec3& mx = local.max;
	glm::vec3 corners[8] = {
	    {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
	    {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}
	};
	glm::vec3 min_v = glm::vec3(model * glm::vec4(corners[0], 1.0f));
	glm::vec3 max_v = min_v;
	for (int i = 1; i < 8; ++i) {
		glm::vec3 p = glm::vec3(model * glm::vec4(corners[i], 1.0f));
		min_v = glm::min(min_v, p);
		max_v = glm::max(max_v, p);
	}
	return {min_v, max_v};
}

VeMesh::VeMesh(VeDevice& device, const std::string& resource_id,
               const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
	: Resource(resource_id), m_ve_device(device) {
	computeLocalAABB(vertices);
	createVertexBuffers(vertices);
	createShadowVertexBuffer(vertices);
	createIndexBuffers(indices);
	m_cpu_positions.resize(vertices.size());
	for (size_t i = 0; i < vertices.size(); i++)
		m_cpu_positions[i] = vertices[i].pos;
	m_cpu_indices = indices;
	setLoaded(true);
}

VeMesh::VeMesh(VeDevice& device, const std::string& resource_id,
               const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
               const std::vector<std::vector<uint32_t>>& lod_indices)
	: Resource(resource_id), m_ve_device(device) {
	computeLocalAABB(vertices);
	createVertexBuffers(vertices);
	createShadowVertexBuffer(vertices);
	createIndexBuffers(indices);
	for (const auto& lod : lod_indices)
		createLodIndexBuffer(lod);
	m_cpu_positions.resize(vertices.size());
	for (size_t i = 0; i < vertices.size(); i++)
		m_cpu_positions[i] = vertices[i].pos;
	m_cpu_indices = indices;
	setLoaded(true);
}

VeMesh::~VeMesh() {
	unload();
}

void VeMesh::setMeshletData(std::unique_ptr<CpuMeshletData> data) {
	m_meshlet_data = std::move(data);
}

std::unique_ptr<CpuMeshletData> VeMesh::buildMeshletData(
	const std::vector<Vertex>& vertices,
	const std::vector<uint32_t>& base_indices,
	const std::vector<std::vector<uint32_t>>& lod_indices) {

	if (vertices.empty() || base_indices.empty())
		return nullptr;

	auto data = std::make_unique<CpuMeshletData>();
	const float* positions = &vertices[0].pos.x;
	size_t vertex_count = vertices.size();

	std::vector<const std::vector<uint32_t>*> all_lods;
	all_lods.push_back(&base_indices);
	for (auto& li : lod_indices)
		all_lods.push_back(&li);

	for (auto* idx_buf : all_lods) {
		CpuMeshletLod lod_data;
		size_t index_count = idx_buf->size();
		size_t max_m = meshopt_buildMeshletsBound(index_count, MESHLET_MAX_VERTICES, MESHLET_MAX_TRIANGLES);
		std::vector<meshopt_Meshlet> meshlets(max_m);
		std::vector<unsigned int> m_verts(max_m * MESHLET_MAX_VERTICES);
		std::vector<unsigned char> m_tris(max_m * MESHLET_MAX_TRIANGLES * 3);

		size_t m_count = meshopt_buildMeshlets(
			meshlets.data(), m_verts.data(), m_tris.data(),
			idx_buf->data(), index_count,
			positions, vertex_count, sizeof(Vertex),
			MESHLET_MAX_VERTICES, MESHLET_MAX_TRIANGLES, 0.5f);
		meshlets.resize(m_count);

		uint32_t local_index_offset = 0;
		for (size_t mi = 0; mi < m_count; mi++) {
			const auto& m = meshlets[mi];
			meshopt_Bounds b = meshopt_computeMeshletBounds(
				m_verts.data() + m.vertex_offset,
				m_tris.data() + m.triangle_offset,
				m.triangle_count,
				positions, vertex_count, sizeof(Vertex));

			uint32_t tri_index_count = m.triangle_count * 3;
			lod_data.meshlets.push_back({
				.bounding_sphere  = glm::vec4(b.center[0], b.center[1], b.center[2], b.radius),
				.cone_apex        = glm::vec4(b.cone_apex[0], b.cone_apex[1], b.cone_apex[2], 0.0f),
				.cone_axis_cutoff = glm::vec4(b.cone_axis[0], b.cone_axis[1], b.cone_axis[2], b.cone_cutoff),
				.index_offset     = local_index_offset,
				.index_count      = tri_index_count,
				.vertex_offset    = 0,
				._pad             = 0,
			});

			for (uint32_t ti = 0; ti < m.triangle_count; ti++)
				for (uint32_t j = 0; j < 3; j++)
					lod_data.indices.push_back(m_verts[m.vertex_offset + m_tris[m.triangle_offset + ti * 3 + j]]);
			local_index_offset += tri_index_count;
		}
		data->lods.push_back(std::move(lod_data));
	}
	return data;
}

bool VeMesh::doLoad() {
	// Meshes are created from data, not loaded from file
	return true;
}

void VeMesh::doUnload() {
	m_vertex_buffer.reset();
	m_shadow_vertex_buffer.reset();
	m_index_buffer.reset();
	m_lod_levels.clear();
	m_cpu_positions.clear();
	m_cpu_positions.shrink_to_fit();
	m_cpu_indices.clear();
	m_cpu_indices.shrink_to_fit();
	m_vertex_count = 0;
	m_index_count = 0;
}

void VeMesh::computeLocalAABB(const std::vector<Vertex>& vertices) {
	if (vertices.empty()) {
		m_local_aabb = {glm::vec3{0.0f}, glm::vec3{0.0f}};
		return;
	}
	glm::vec3 min_v = vertices[0].pos;
	glm::vec3 max_v = vertices[0].pos;
	for (size_t i = 1; i < vertices.size(); ++i) {
		const glm::vec3& p = vertices[i].pos;
		min_v = glm::min(min_v, p);
		max_v = glm::max(max_v, p);
	}
	m_local_aabb = {min_v, max_v};
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
	staging_buffer.writeToBuffer(vertices.data());

	m_vertex_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(vertices[0]),
		m_vertex_count,
		vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		1
	);
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_vertex_buffer->getBuffer(),
	                       sizeof(vertices[0]) * m_vertex_count);
}

void VeMesh::createShadowVertexBuffer(const std::vector<Vertex>& vertices) {
	// Position-only buffer for shadow passes (12 bytes/vertex vs 48 bytes/vertex)
	std::vector<glm::vec3> positions(vertices.size());
	for (size_t i = 0; i < vertices.size(); i++) {
		positions[i] = vertices[i].pos;
	}

	VeBuffer staging_buffer(
		m_ve_device,
		sizeof(glm::vec3),
		m_vertex_count,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(positions.data());

	m_shadow_vertex_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(glm::vec3),
		m_vertex_count,
		vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		1
	);
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_shadow_vertex_buffer->getBuffer(),
	                       sizeof(glm::vec3) * m_vertex_count);
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
	staging_buffer.writeToBuffer(indices.data());

	m_index_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(indices[0]),
		m_index_count,
		vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		1
	);
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_index_buffer->getBuffer(),
	                       sizeof(indices[0]) * m_index_count);
}

void VeMesh::createLodIndexBuffer(const std::vector<uint32_t>& indices) {
	LodLevel lod;
	lod.index_count = static_cast<uint32_t>(indices.size());

	VeBuffer staging_buffer(
		m_ve_device,
		sizeof(indices[0]),
		lod.index_count,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(indices.data());

	lod.index_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(indices[0]),
		lod.index_count,
		vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		1
	);
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), lod.index_buffer->getBuffer(),
	                       sizeof(indices[0]) * lod.index_count);

	m_lod_levels.push_back(std::move(lod));
}

uint32_t VeMesh::getLodIndexCount(uint32_t lod) const {
	if (lod == 0)
		return m_index_count;
	assert(lod - 1 < m_lod_levels.size());
	return m_lod_levels[lod - 1].index_count;
}

VeBuffer& VeMesh::getLodIndexBuffer(uint32_t lod) const {
	if (lod == 0)
		return *m_index_buffer;
	assert(lod - 1 < m_lod_levels.size());
	return *m_lod_levels[lod - 1].index_buffer;
}

void VeMesh::bindLodIndexBuffer(vk::raii::CommandBuffer& command_buffer, uint32_t lod) const {
	command_buffer.bindIndexBuffer(*getLodIndexBuffer(lod).getBuffer(), 0, vk::IndexType::eUint32);
}

void VeMesh::drawIndexedLod(vk::raii::CommandBuffer& command_buffer, uint32_t lod,
                            uint32_t instance_count, uint32_t first_instance) const {
	command_buffer.drawIndexed(getLodIndexCount(lod), instance_count, 0, 0, first_instance);
}

void VeMesh::bindVertexBuffer(vk::raii::CommandBuffer& command_buffer) const {
	vk::Buffer buffers[] = {*m_vertex_buffer->getBuffer()};
	vk::DeviceSize offsets[] = {0};
	command_buffer.bindVertexBuffers(0, buffers, offsets);
}

void VeMesh::bindShadowVertexBuffer(vk::raii::CommandBuffer& command_buffer) const {
	vk::Buffer buffers[] = {*m_shadow_vertex_buffer->getBuffer()};
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

void VeMesh::drawIndexed(vk::raii::CommandBuffer& command_buffer, uint32_t instance_count, uint32_t first_instance) const {
	command_buffer.drawIndexed(m_index_count, instance_count, 0, 0, first_instance);
}

std::vector<vk::VertexInputBindingDescription> VeMesh::Vertex::getBindingDescriptions() {
	return {vk::VertexInputBindingDescription{
		.binding = 0,
		.stride = sizeof(Vertex),
		.inputRate = vk::VertexInputRate::eVertex
	}};
}

std::vector<vk::VertexInputBindingDescription> VeMesh::Vertex::getShadowBindingDescriptions() {
	return {vk::VertexInputBindingDescription{
		.binding = 0,
		.stride = sizeof(glm::vec3),  // position-only (12 bytes)
		.inputRate = vk::VertexInputRate::eVertex
	}};
}

std::vector<vk::VertexInputAttributeDescription> VeMesh::Vertex::getAttributeDescriptionsSimple() {
	return {
		vk::VertexInputAttributeDescription{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, pos)},
		vk::VertexInputAttributeDescription{.location = 1, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, normal)},
		vk::VertexInputAttributeDescription{.location = 2, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(Vertex, tex_coord)}
	};
}

std::vector<vk::VertexInputAttributeDescription> VeMesh::Vertex::getAttributeDescriptions() {
	return {
		vk::VertexInputAttributeDescription{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, pos)},
		vk::VertexInputAttributeDescription{.location = 1, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, normal)},
		vk::VertexInputAttributeDescription{.location = 2, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(Vertex, tex_coord)},
		vk::VertexInputAttributeDescription{.location = 3, .binding = 0, .format = vk::Format::eR32G32B32A32Sfloat, .offset = offsetof(Vertex, tangent)}
	};
}

std::vector<vk::VertexInputAttributeDescription> VeMesh::Vertex::getAttributeDescriptionsShadow() {
	return {vk::VertexInputAttributeDescription{
		.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 0
	}};
}

} // namespace ve
