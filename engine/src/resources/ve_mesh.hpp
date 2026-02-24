/* VeMesh - geometry only (vertex and index buffers).
 * Inherits from Resource for use with VeResourceManager.
 * Used as a building block for scene-graph models.
 * Contains vertex and index buffers and methods to bind and draw them.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_buffer.hpp"

#define GLM_FORCE_RADIANS
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <filesystem>
#include <memory>
#include <vector>

namespace ve {

class VENGINE_API VeMesh : public Resource {
public:
	struct AABB {
		glm::vec3 min;
		glm::vec3 max;
	};

	struct Vertex {
		glm::vec3 pos;
		glm::vec3 normal;
		glm::vec2 tex_coord{0.0f, 0.0f};
		glm::vec4 tangent{0.0f, 0.0f, 0.0f, 0.0f};

		static std::vector<vk::VertexInputBindingDescription> getBindingDescriptions();
		static std::vector<vk::VertexInputBindingDescription> getShadowBindingDescriptions();
		static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptions();
		static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptionsSimple();
		static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptionsShadow();

		bool operator==(const Vertex& other) const {
			return pos == other.pos && normal == other.normal &&
			       tangent == other.tangent && tex_coord == other.tex_coord;
		}
	};

	struct LodLevel {
		std::unique_ptr<VeBuffer> index_buffer;
		uint32_t index_count = 0;
	};

	// Create mesh from vertices and indices, extracted from glTF model for example.
	VeMesh(VeDevice& device, const std::string& resource_id,
	       const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
	// Create mesh with additional LOD index buffers (lod_indices[0] = LOD 1, etc.)
	VeMesh(VeDevice& device, const std::string& resource_id,
	       const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
	       const std::vector<std::vector<uint32_t>>& lod_indices);
	~VeMesh() override;

	VeMesh(const VeMesh&) = delete;
	VeMesh& operator=(const VeMesh&) = delete;

	void bindVertexBuffer(vk::raii::CommandBuffer& command_buffer) const;
	void bindShadowVertexBuffer(vk::raii::CommandBuffer& command_buffer) const;
	void bindIndexBuffer(vk::raii::CommandBuffer& command_buffer) const;
	void bindLodIndexBuffer(vk::raii::CommandBuffer& command_buffer, uint32_t lod) const;
	void draw(vk::raii::CommandBuffer& command_buffer) const;
	void drawIndexed(vk::raii::CommandBuffer& command_buffer) const;
	void drawIndexed(vk::raii::CommandBuffer& command_buffer, uint32_t instance_count, uint32_t first_instance) const;
	void drawIndexedLod(vk::raii::CommandBuffer& command_buffer, uint32_t lod,
	                    uint32_t instance_count, uint32_t first_instance) const;

	uint32_t getVertexCount() const { return m_vertex_count; }
	uint32_t getIndexCount() const { return m_index_count; }
	uint32_t getLodCount() const { return 1 + static_cast<uint32_t>(m_lod_levels.size()); }
	uint32_t getLodIndexCount(uint32_t lod) const;
	VeBuffer& getLodIndexBuffer(uint32_t lod) const;
	VeBuffer& getShadowVertexBuffer() const { return *m_shadow_vertex_buffer; }
	VeBuffer& getIndexBuffer() const { return *m_index_buffer; }

	AABB getLocalAABB() const { return m_local_aabb; }

	const std::vector<glm::vec3>& getCpuPositions() const { return m_cpu_positions; }
	const std::vector<uint32_t>& getCpuIndices() const { return m_cpu_indices; }
	bool hasCpuGeometry() const { return !m_cpu_positions.empty(); }

protected:
	bool doLoad() override;
	void doUnload() override;

private:
	void createVertexBuffers(const std::vector<Vertex>& vertices);
	void createShadowVertexBuffer(const std::vector<Vertex>& vertices);
	void createIndexBuffers(const std::vector<uint32_t>& indices);
	void createLodIndexBuffer(const std::vector<uint32_t>& indices);
	void computeLocalAABB(const std::vector<Vertex>& vertices);

	VeDevice& m_ve_device;
	std::unique_ptr<VeBuffer> m_vertex_buffer;
	std::unique_ptr<VeBuffer> m_shadow_vertex_buffer;
	std::unique_ptr<VeBuffer> m_index_buffer;
	std::vector<LodLevel> m_lod_levels;  // LODs 1+ (LOD 0 is m_index_buffer)
	uint32_t m_vertex_count{0};
	uint32_t m_index_count{0};
	AABB m_local_aabb{};
	std::vector<glm::vec3> m_cpu_positions;
	std::vector<uint32_t> m_cpu_indices;
};

// Transform AABB by model matrix (transform 8 corners, take min/max of result).
VENGINE_API VeMesh::AABB transformAABB(const VeMesh::AABB& local, const glm::mat4& model);

} // namespace ve

template<>
struct std::hash<ve::VeMesh::Vertex> {
	size_t operator()(ve::VeMesh::Vertex const& v) const noexcept {
		size_t seed = 0u;
		seed ^= std::hash<glm::vec3>()(v.pos) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= std::hash<glm::vec3>()(v.normal) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= std::hash<glm::vec2>()(v.tex_coord) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= std::hash<glm::vec4>()(v.tangent) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		return seed;
	}
};
