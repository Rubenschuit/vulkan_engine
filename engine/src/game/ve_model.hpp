/* VeModel is responsible for managing the vertex and index buffers
for a model. It provides methods to create and bind these buffers and issue
draw commands. */
#pragma once
#include "core/ve_descriptors.hpp"
#include "ve_export.hpp"
#include "core/ve_device.hpp"
#include "core/ve_texture.hpp"
#include "core/ve_buffer.hpp"

namespace ve {
	class VeDescriptorSetLayout;
	class VeDescriptorPool;
}

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <filesystem>

namespace ve {

class VENGINE_API VeModel {
public:
	struct Vertex {
		glm::vec3 pos;
		glm::vec3 color;
		glm::vec3 normal;
		glm::vec2 tex_coord{0.0f, 0.0f};
		glm::vec4 tangent{0.0f, 0.0f, 0.0f, 0.0f};
		uint32_t material_index{0}; // default to 0 for simple models

		static std::vector<vk::VertexInputBindingDescription> getBindingDescriptions();
		// for models with material index
		static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptions();
		// for simple models with no material index
		static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptionsSimple();
		// for shadow mapping (only position)
		static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptionsShadow();
		bool operator==(const Vertex& other) const {
			return pos == other.pos &&
			       color == other.color &&
				   normal == other.normal &&
				   tangent == other.tangent &&
				   tex_coord == other.tex_coord &&
				   material_index == other.material_index;
		}
	};

	// Do these need to be shared ptr?
	struct MaterialTextures {
		std::shared_ptr<VeTexture> albedo_texture{nullptr};
		std::shared_ptr<VeTexture> normal_texture{nullptr};
		std::shared_ptr<VeTexture> metallic_roughness_texture{nullptr};
	};


	VeModel(VeDevice& device, const std::vector<Vertex>& vertices);
	VeModel(VeDevice& device, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
	VeModel(VeDevice& device, const std::filesystem::path& model_path);
	~VeModel();

	VeModel(const VeModel&) = delete;
	VeModel& operator=(const VeModel&) = delete;

	void bindVertexBuffer(vk::raii::CommandBuffer& commandBuffer);
	void bindIndexBuffer(vk::raii::CommandBuffer& commandBuffer);
	void draw(vk::raii::CommandBuffer& commandBuffer);
	void drawIndexed(vk::raii::CommandBuffer& commandBuffer);

	void createDescriptorSet(VeDescriptorPool& pool, VeDescriptorSetLayout& set_layout);
	// not safe yet
	vk::raii::DescriptorSet& getMaterialDescriptorSet() { return m_material_descriptor_set; }
	bool hasTexturedMaterials() const { return m_has_textured_materials; }

private:
	void createVertexBuffers(const std::vector<Vertex>& vertices);
	void createIndexBuffers(const std::vector<uint32_t>& indices);
	void createMaterialTextures(
		const std::vector<std::filesystem::path>& albedo_paths,
		const std::vector<std::filesystem::path>& normal_paths,
		const std::vector<std::filesystem::path>& metallic_roughness_paths
	);


	VeDevice& m_ve_device; // not owned, must outlive model

	// TODO: Consdider consolidating index and vertex buffer into single buffer and use offsets
	std::unique_ptr<ve::VeBuffer> m_vertex_buffer;
	uint32_t m_vertex_count;
	std::unique_ptr<ve::VeBuffer> m_index_buffer;
	uint32_t m_index_count;

	// Material information
	MaterialTextures m_material_textures;
	vk::raii::DescriptorSet m_material_descriptor_set{nullptr};
	bool m_has_textured_materials{false};
};

} // namespace ve

// Provide a hash function for Vertex so we can use it in unordered_map
// Needs to be outside the ve namespace because std is not allowed to be extended inside another namespace
template<> struct std::hash<ve::VeModel::Vertex> {
	size_t operator()(ve::VeModel::Vertex const& v) const noexcept {
		size_t seed = 0u;
		seed ^= std::hash<glm::vec3>()(v.pos) + 0x9e3779b9 + (seed<<6) + (seed>>2);
		seed ^= std::hash<glm::vec3>()(v.color) + 0x9e3779b9 + (seed<<6) + (seed>>2);
		seed ^= std::hash<glm::vec3>()(v.normal) + 0x9e3779b9 + (seed<<6) + (seed>>2);
		seed ^= std::hash<glm::vec2>()(v.tex_coord) + 0x9e3779b9 + (seed<<6) + (seed>>2);
		seed ^= std::hash<glm::vec4>()(v.tangent) + 0x9e3779b9 + (seed<<6) + (seed>>2);
		seed ^= std::hash<uint32_t>()(v.material_index) + 0x9e3779b9 + (seed<<6) + (seed>>2);
		return seed;
	}
};
