/* VeModel is responsible for managing the vertex and index buffers
   for a model. It provides methods to bind these buffers and issue
   draw commands. */
#pragma once

#include "ve_device.hpp"
#include "ve_buffer.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>



namespace ve {
	class VeModel {
	public:
		struct Vertex {
			glm::vec3 pos;
			glm::vec3 color;
			glm::vec3 normal;
			glm::vec2 tex_coord;

			static std::vector<vk::VertexInputBindingDescription> getBindingDescriptions();
			static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptions();
			bool operator==(const Vertex& other) const {
				// Include normal in equality so that flat vs. smooth shaded meshes deduplicate differently
				return pos == other.pos && color == other.color && normal == other.normal && tex_coord == other.tex_coord;
			}
		};

		VeModel(VeDevice& device, const std::vector<Vertex>& vertices);
		VeModel(VeDevice& device, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
		VeModel(VeDevice& device, char const* model_path);
		~VeModel();

		VeModel(const VeModel&) = delete;
		VeModel& operator=(const VeModel&) = delete;

		void bindVertexBuffer(vk::raii::CommandBuffer& commandBuffer);
		void bindIndexBuffer(vk::raii::CommandBuffer& commandBuffer);
		void draw(vk::raii::CommandBuffer& commandBuffer);
		void drawIndexed(vk::raii::CommandBuffer& commandBuffer);

	private:
		void createVertexBuffers(const std::vector<Vertex>& vertices);
		void createIndexBuffers(const std::vector<uint32_t>& indices);

		VeDevice& ve_device; // not owned, must outlive model

		std::unique_ptr<ve::VeBuffer> vertex_buffer;
		uint32_t vertex_count;

		// TODO: Consdider consolidating index and vertex buffer into single buffer and use offsets
		std::unique_ptr<ve::VeBuffer> index_buffer;
		uint32_t index_count;
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
		return seed;
	}
};
