#include "pch.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "rendering/culling/meshlet_data.hpp"
#include "resources/ve_mesh.hpp"
#include "utils/ve_log.hpp"

#include <algorithm>

namespace ve {

PbrMegaBuffer::PbrMegaBuffer(VeDevice& device) : m_ve_device(device) {}
PbrMegaBuffer::~PbrMegaBuffer() = default;

void PbrMegaBuffer::build(vk::raii::CommandBuffer& cmd, const std::vector<VeMesh*>& meshes) {
	m_entries.clear();
	m_meshlet_entries.clear();
	m_mega_vbo.reset();
	m_mega_shadow_vbo.reset();
	m_mega_ibo.reset();
	m_meshlet_ibo.reset();
	m_meshlet_ssbo.reset();
	m_meshlet_ssbo_staging.reset();
	m_meshlet_ibo_staging.reset();

	if (meshes.empty())
		return;

	uint32_t total_vertices = 0;
	uint32_t total_indices = 0;
	for (VeMesh* mesh : meshes) {
		total_vertices += mesh->getVertexCount();
		for (uint32_t lod = 0; lod < mesh->getLodCount(); lod++)
			total_indices += mesh->getLodIndexCount(lod);
	}

	if (total_vertices == 0 || total_indices == 0)
		return;

	m_mega_vbo = std::make_unique<VeBuffer>(m_ve_device,
		sizeof(VeMesh::Vertex), total_vertices,
		vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal);

	m_mega_shadow_vbo = std::make_unique<VeBuffer>(m_ve_device,
		sizeof(glm::vec3), total_vertices,
		vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal);

	m_mega_ibo = std::make_unique<VeBuffer>(m_ve_device,
		sizeof(uint32_t), total_indices,
		vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal);

	uint32_t vertex_offset = 0;
	uint32_t index_offset = 0;

	for (VeMesh* mesh : meshes) {
		uint32_t vc = mesh->getVertexCount();

		vk::BufferCopy vbo_copy{
			.srcOffset = 0,
			.dstOffset = static_cast<vk::DeviceSize>(vertex_offset) * sizeof(VeMesh::Vertex),
			.size = static_cast<vk::DeviceSize>(vc) * sizeof(VeMesh::Vertex)
		};
		cmd.copyBuffer(mesh->getVertexBuffer().getBuffer(),
			m_mega_vbo->getBuffer(), vbo_copy);

		vk::BufferCopy shadow_vbo_copy{
			.srcOffset = 0,
			.dstOffset = static_cast<vk::DeviceSize>(vertex_offset) * sizeof(glm::vec3),
			.size = static_cast<vk::DeviceSize>(vc) * sizeof(glm::vec3)
		};
		cmd.copyBuffer(mesh->getShadowVertexBuffer().getBuffer(),
			m_mega_shadow_vbo->getBuffer(), shadow_vbo_copy);

		MeshEntry entry;
		entry.vertex_offset = vertex_offset;
		entry.lod_entries.reserve(mesh->getLodCount());

		for (uint32_t lod = 0; lod < mesh->getLodCount(); lod++) {
			uint32_t ic = mesh->getLodIndexCount(lod);
			vk::BufferCopy ibo_copy{
				.srcOffset = 0,
				.dstOffset = static_cast<vk::DeviceSize>(index_offset) * sizeof(uint32_t),
				.size = static_cast<vk::DeviceSize>(ic) * sizeof(uint32_t)
			};
			cmd.copyBuffer(mesh->getLodIndexBuffer(lod).getBuffer(),
				m_mega_ibo->getBuffer(), ibo_copy);

			entry.lod_entries.push_back({index_offset, ic});
			index_offset += ic;
		}

		m_entries[mesh] = std::move(entry);
		vertex_offset += vc;
	}

	VE_LOGI("PBR mega-buffer: " << meshes.size() << " meshes, "
		<< total_vertices << " verts, " << total_indices << " indices");

	// --- Meshlet data ---
	uint32_t total_meshlet_count = 0;
	uint32_t total_meshlet_indices = 0;
	for (VeMesh* mesh : meshes) {
		const CpuMeshletData* md = mesh->getMeshletData();
		if (!md) continue;
		for (const auto& lod : md->lods) {
			total_meshlet_count += static_cast<uint32_t>(lod.meshlets.size());
			total_meshlet_indices += static_cast<uint32_t>(lod.indices.size());
		}
	}

	if (total_meshlet_count == 0 || total_meshlet_indices == 0) {
		VE_LOGI("PBR mega-buffer: no meshlet data available");
		return;
	}

	// Accumulate all MeshletGPU and meshlet indices into flat CPU arrays, patching global offsets
	std::vector<MeshletGPU> all_meshlets;
	all_meshlets.reserve(total_meshlet_count);
	std::vector<uint32_t> all_meshlet_indices;
	all_meshlet_indices.reserve(total_meshlet_indices);

	for (VeMesh* mesh : meshes) {
		const CpuMeshletData* md = mesh->getMeshletData();
		const MeshEntry* entry = getEntry(mesh);
		uint32_t global_vertex_offset = entry ? entry->vertex_offset : 0;

		MeshletMeshEntry meshlet_entry;
		if (md) {
			for (const auto& lod : md->lods) {
				uint32_t lod_ssbo_start = static_cast<uint32_t>(all_meshlets.size());
				uint32_t lod_ibo_start  = static_cast<uint32_t>(all_meshlet_indices.size());
				uint32_t lod_meshlet_count = static_cast<uint32_t>(lod.meshlets.size());

				for (const auto& m : lod.meshlets) {
					MeshletGPU patched = m;
					patched.index_offset  = lod_ibo_start + m.index_offset;
					patched.vertex_offset = global_vertex_offset;
					all_meshlets.push_back(patched);
				}
				for (uint32_t idx : lod.indices)
					all_meshlet_indices.push_back(idx);

				meshlet_entry.lod_entries.push_back({lod_ssbo_start, lod_meshlet_count});
			}
		}
		m_meshlet_entries[mesh] = std::move(meshlet_entry);
	}

	// Upload via staging buffers
	m_meshlet_ssbo_staging = std::make_unique<VeBuffer>(m_ve_device,
		sizeof(MeshletGPU), total_meshlet_count,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	m_meshlet_ssbo_staging->map();
	m_meshlet_ssbo_staging->writeToBuffer(all_meshlets.data());

	m_meshlet_ibo_staging = std::make_unique<VeBuffer>(m_ve_device,
		sizeof(uint32_t), total_meshlet_indices,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	m_meshlet_ibo_staging->map();
	m_meshlet_ibo_staging->writeToBuffer(all_meshlet_indices.data());

	m_meshlet_ssbo = std::make_unique<VeBuffer>(m_ve_device,
		sizeof(MeshletGPU), total_meshlet_count,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal);

	m_meshlet_ibo = std::make_unique<VeBuffer>(m_ve_device,
		sizeof(uint32_t), total_meshlet_indices,
		vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal);

	cmd.copyBuffer(m_meshlet_ssbo_staging->getBuffer(), m_meshlet_ssbo->getBuffer(),
		vk::BufferCopy{0, 0, sizeof(MeshletGPU) * total_meshlet_count});
	cmd.copyBuffer(m_meshlet_ibo_staging->getBuffer(), m_meshlet_ibo->getBuffer(),
		vk::BufferCopy{0, 0, sizeof(uint32_t) * total_meshlet_indices});

	VE_LOGI("PBR mega-buffer meshlets: " << total_meshlet_count << " meshlets, "
		<< total_meshlet_indices << " indices");
}

void PbrMegaBuffer::clear() {
	m_entries.clear();
	m_meshlet_entries.clear();
	m_mega_vbo.reset();
	m_mega_shadow_vbo.reset();
	m_mega_ibo.reset();
	m_meshlet_ibo.reset();
	m_meshlet_ssbo.reset();
	m_meshlet_ssbo_staging.reset();
	m_meshlet_ibo_staging.reset();
}

const PbrMegaBuffer::MeshEntry* PbrMegaBuffer::getEntry(VeMesh* mesh) const {
	auto it = m_entries.find(mesh);
	return (it != m_entries.end()) ? &it->second : nullptr;
}

const PbrMegaBuffer::MeshletMeshEntry* PbrMegaBuffer::getMeshletEntry(VeMesh* mesh) const {
	auto it = m_meshlet_entries.find(mesh);
	return (it != m_meshlet_entries.end()) ? &it->second : nullptr;
}

void PbrMegaBuffer::bind(vk::raii::CommandBuffer& cmd) const {
	if (!m_mega_vbo || !m_mega_ibo)
		return;
	vk::Buffer buffers[] = {m_mega_vbo->getBuffer()};
	vk::DeviceSize offsets[] = {0};
	cmd.bindVertexBuffers(0, buffers, offsets);
	cmd.bindIndexBuffer(m_mega_ibo->getBuffer(), 0, vk::IndexType::eUint32);
}

void PbrMegaBuffer::bindShadow(vk::raii::CommandBuffer& cmd) const {
	if (!m_mega_shadow_vbo || !m_mega_ibo)
		return;
	vk::Buffer buffers[] = {m_mega_shadow_vbo->getBuffer()};
	vk::DeviceSize offsets[] = {0};
	cmd.bindVertexBuffers(0, buffers, offsets);
	cmd.bindIndexBuffer(m_mega_ibo->getBuffer(), 0, vk::IndexType::eUint32);
}

void PbrMegaBuffer::bindMeshletIbo(vk::raii::CommandBuffer& cmd) const {
	if (!m_mega_vbo || !m_meshlet_ibo)
		return;
	vk::Buffer buffers[] = {m_mega_vbo->getBuffer()};
	vk::DeviceSize offsets[] = {0};
	cmd.bindVertexBuffers(0, buffers, offsets);
	cmd.bindIndexBuffer(m_meshlet_ibo->getBuffer(), 0, vk::IndexType::eUint32);
}

void PbrMegaBuffer::bindShadowMeshletIbo(vk::raii::CommandBuffer& cmd) const {
	if (!m_mega_shadow_vbo || !m_meshlet_ibo)
		return;
	vk::Buffer buffers[] = {m_mega_shadow_vbo->getBuffer()};
	vk::DeviceSize offsets[] = {0};
	cmd.bindVertexBuffers(0, buffers, offsets);
	cmd.bindIndexBuffer(m_meshlet_ibo->getBuffer(), 0, vk::IndexType::eUint32);
}

} // namespace ve
