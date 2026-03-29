#pragma once
#include "ve_export.hpp"
#include "vulkan/ve_buffer.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

class VeDevice;
class VeMesh;

class VENGINE_API PbrMegaBuffer {
public:
	struct LodEntry {
		uint32_t first_index;
		uint32_t index_count;
	};
	struct MeshEntry {
		uint32_t vertex_offset;
		std::vector<LodEntry> lod_entries;
	};

	struct MeshletLodEntry {
		uint32_t meshlet_offset;  // start index in global meshlet SSBO
		uint32_t meshlet_count;
	};
	struct MeshletMeshEntry {
		std::vector<MeshletLodEntry> lod_entries;
	};

	explicit PbrMegaBuffer(VeDevice& device);
	~PbrMegaBuffer();

	PbrMegaBuffer(const PbrMegaBuffer&) = delete;
	PbrMegaBuffer& operator=(const PbrMegaBuffer&) = delete;

	// Build the mega buffer from all scene meshes. Call once at scene load.
	// cmd must be a recording command buffer (copies are GPU-side).
	void build(vk::raii::CommandBuffer& cmd, const std::vector<VeMesh*>& meshes);

	const MeshEntry* getEntry(VeMesh* mesh) const;
	const MeshletMeshEntry* getMeshletEntry(VeMesh* mesh) const;

	bool isValid() const { return m_mega_vbo != nullptr; }
	bool hasMeshletData() const { return m_meshlet_ssbo != nullptr; }

	VeBuffer* getMeshletSsbo() const { return m_meshlet_ssbo.get(); }

	void clear();
	void bind(vk::raii::CommandBuffer& cmd) const;
	void bindShadow(vk::raii::CommandBuffer& cmd) const;
	void bindMeshletIbo(vk::raii::CommandBuffer& cmd) const;
	void bindShadowMeshletIbo(vk::raii::CommandBuffer& cmd) const;

private:
	VeDevice& m_ve_device;

	std::unique_ptr<VeBuffer> m_mega_vbo;
	std::unique_ptr<VeBuffer> m_mega_shadow_vbo;
	std::unique_ptr<VeBuffer> m_mega_ibo;

	std::unique_ptr<VeBuffer> m_meshlet_ibo;
	std::unique_ptr<VeBuffer> m_meshlet_ssbo;
	// Staging buffers kept alive until endSingleTimeCommands completes
	std::unique_ptr<VeBuffer> m_meshlet_ssbo_staging;
	std::unique_ptr<VeBuffer> m_meshlet_ibo_staging;

	std::unordered_map<VeMesh*, MeshEntry> m_entries;
	std::unordered_map<VeMesh*, MeshletMeshEntry> m_meshlet_entries;
};

} // namespace ve
