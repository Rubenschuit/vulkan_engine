#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "events/event_bus.hpp"
#include "vulkan/ve_buffer.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

class VeDevice;
class VeMesh;

class VENGINE_API PbrMegaBuffer {
public:
	static constexpr uint32_t NO_SKIN_OFFSET = UINT32_MAX;
	static constexpr uint32_t NO_MORPH_OFFSET = UINT32_MAX;

	struct LodEntry {
		uint32_t first_index;
		uint32_t index_count;
	};
	struct MeshEntry {
		uint32_t vertex_offset;
		uint32_t skin_vertex_offset = NO_SKIN_OFFSET;
		uint32_t morph_offset = NO_MORPH_OFFSET;
		uint32_t morph_target_count = 0;
		std::vector<LodEntry> lod_entries;
	};

	struct MeshletLodEntry {
		uint32_t meshlet_offset;  // start index in global meshlet SSBO
		uint32_t meshlet_count;
	};
	struct MeshletMeshEntry {
		std::vector<MeshletLodEntry> lod_entries;
	};

	PbrMegaBuffer(VeDevice& device, EventBus& event_bus);
	~PbrMegaBuffer();

	PbrMegaBuffer(const PbrMegaBuffer&) = delete;
	PbrMegaBuffer& operator=(const PbrMegaBuffer&) = delete;

	// Builds the mega buffer from `meshes`. When `previous` is non-null, meshes
	// present in `previous->m_entries` are sourced via GPU->GPU copy from the
	// previous mega buffers (since per-mesh GPU buffers are released after the
	// initial build); other meshes are copied from their own per-mesh buffers.
	void build(vk::raii::CommandBuffer& cmd, const std::vector<VeMesh*>& meshes,
	           const PbrMegaBuffer* previous = nullptr);

	// Swaps all GPU buffers + lookup tables with `other`
	void swapState(PbrMegaBuffer& other) noexcept;

	const MeshEntry* getEntry(VeMesh* mesh) const;
	const MeshletMeshEntry* getMeshletEntry(VeMesh* mesh) const;

	bool isValid() const { return m_mega_vbo != nullptr; }
	bool hasMeshletData() const { return m_meshlet_ssbo != nullptr; }
	bool hasSkinData() const { return m_mega_skin_vbo != nullptr; }
	bool hasMorphData() const { return m_mega_morph_position != nullptr; }

	// Monotonic tag bumped on every (re)build/swap
	uint64_t generation() const { return m_generation; }

	VeBuffer* getMeshletSsbo() const { return m_meshlet_ssbo.get(); }
	VeBuffer* getMegaVbo() const { return m_mega_vbo.get(); }
	VeBuffer* getMegaShadowVbo() const { return m_mega_shadow_vbo.get(); }
	VeBuffer* getMegaSkinVbo() const { return m_mega_skin_vbo.get(); }
	VeBuffer* getMegaMorphPosition() const { return m_mega_morph_position.get(); }
	VeBuffer* getMegaMorphNormal() const { return m_mega_morph_normal.get(); }

	uint32_t getDynamicRegionBase() const { return m_static_vertex_count; }
	uint32_t getDynamicRegionCapacity() const { return MAX_DEFORMED_VERTICES_PER_FRAME; }

	void clear();
	void bind(vk::raii::CommandBuffer& cmd) const;
	void bindShadow(vk::raii::CommandBuffer& cmd) const;
	void bindIbo(vk::raii::CommandBuffer& cmd) const;
	void bindMeshletIbo(vk::raii::CommandBuffer& cmd) const;
	void bindShadowMeshletIbo(vk::raii::CommandBuffer& cmd) const;

private:
	VeDevice& m_ve_device;
	EventBus& m_event_bus;
	EventSubscriptionId m_mesh_unload_sub = 0;

	std::unique_ptr<VeBuffer> m_mega_vbo;
	std::unique_ptr<VeBuffer> m_mega_shadow_vbo;
	std::unique_ptr<VeBuffer> m_mega_ibo;

	std::unique_ptr<VeBuffer> m_mega_skin_vbo; // Sparse
	uint32_t m_static_vertex_count = 0;
	uint64_t m_generation = 0;

	std::unique_ptr<VeBuffer> m_mega_morph_position;
	std::unique_ptr<VeBuffer> m_mega_morph_normal;

	std::unique_ptr<VeBuffer> m_meshlet_ibo;
	std::unique_ptr<VeBuffer> m_meshlet_ssbo;
	// Staging buffers kept alive until endSingleTimeCommands completes
	std::unique_ptr<VeBuffer> m_meshlet_ssbo_staging;
	std::unique_ptr<VeBuffer> m_meshlet_ibo_staging;

	std::unordered_map<VeMesh*, MeshEntry> m_entries;
	std::unordered_map<VeMesh*, MeshletMeshEntry> m_meshlet_entries;
};

} // namespace ve
