#pragma once

#include "ve_device.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>



namespace ve {
	class VeModel {
	public:

		struct Vertex {
			glm::vec2 pos;
			glm::vec3 color;

			static std::vector<vk::VertexInputBindingDescription> getBindingDescriptions();
			static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptions();
		};

		VeModel(VeDevice& device, const std::vector<Vertex>& vertices);
		VeModel(VeDevice& device, const std::vector<Vertex>& vertices, const std::vector<uint16_t>& indices);
		~VeModel();

		VeModel(const VeModel&) = delete;
		VeModel& operator=(const VeModel&) = delete;

		void bindVertexBuffer(vk::CommandBuffer commandBuffer);
		void bindIndexBuffer(vk::CommandBuffer commandBuffer);
		void draw(vk::CommandBuffer commandBuffer);
		void drawIndexed(vk::CommandBuffer commandBuffer);

	private:
		void createVertexBuffers(const std::vector<Vertex>& vertices);
		void createIndexBuffers(const std::vector<uint16_t>& indices);

		VeDevice& ve_device; // not owned, must outlive model

		vk::raii::Buffer vertex_buffer{nullptr};
		vk::raii::DeviceMemory vertex_buffer_memory{nullptr};
		uint32_t vertex_count;

		// TODO: Consdider consolidating index and vertex buffer into single buffer and use offsets
		vk::raii::Buffer index_buffer{nullptr};
		vk::raii::DeviceMemory index_buffer_memory{nullptr};
		uint32_t index_count;
	};

} // namespace ve
