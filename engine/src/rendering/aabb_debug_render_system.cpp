#include "pch.hpp"
#include "rendering/aabb_debug_render_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_buffer.hpp"
#include "resources/ve_mesh.hpp"
#include "scene/ve_component.hpp"

namespace ve {

namespace {

void addAABBEdges(const VeMesh::AABB& aabb, glm::vec3 color, std::vector<VeMesh::Vertex>& out) {
	const glm::vec3& mn = aabb.min;
	const glm::vec3& mx = aabb.max;
	const glm::vec3 v0(mn.x, mn.y, mn.z);
	const glm::vec3 v1(mx.x, mn.y, mn.z);
	const glm::vec3 v2(mx.x, mx.y, mn.z);
	const glm::vec3 v3(mn.x, mx.y, mn.z);
	const glm::vec3 v4(mn.x, mn.y, mx.z);
	const glm::vec3 v5(mx.x, mn.y, mx.z);
	const glm::vec3 v6(mx.x, mx.y, mx.z);
	const glm::vec3 v7(mn.x, mx.y, mx.z);

	auto push_line = [&](const glm::vec3& a, const glm::vec3& b) {
		out.push_back({a, color, glm::vec3{1.0f}});
		out.push_back({b, color, glm::vec3{1.0f}});
	};

	// Bottom face
	push_line(v0, v1);
	push_line(v1, v2);
	push_line(v2, v3);
	push_line(v3, v0);
	// Top face
	push_line(v4, v5);
	push_line(v5, v6);
	push_line(v6, v7);
	push_line(v7, v4);
	// Vertical edges
	push_line(v0, v4);
	push_line(v1, v5);
	push_line(v2, v6);
	push_line(v3, v7);
}

} // namespace

AabbDebugRenderSystem::AabbDebugRenderSystem(
	VeDevice& device,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	vk::Format color_format,
	vk::SampleCountFlagBits sample_count,
	std::filesystem::path shader_path)
	: m_ve_device(device), m_shader_path(std::move(shader_path)), m_color_format(color_format), m_sample_count(sample_count) {
	createPipelineLayout(global_set_layout);
	createPipeline(color_format, sample_count);
	createVertexBuffer();
}

AabbDebugRenderSystem::~AabbDebugRenderSystem() = default;

void AabbDebugRenderSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout) {
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 1,
		.pSetLayouts = &*global_set_layout,
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void AabbDebugRenderSystem::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.multisample_info.rasterizationSamples = sample_count;
	config.input_assembly_info.topology = vk::PrimitiveTopology::eLineList;
	config.depth_stencil_info.depthTestEnable = VK_TRUE;
	config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	config.color_format = color_format;
	config.pipeline_layout = *m_pipeline_layout;
	config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptionsSimple();
	config.binding_descriptions = VeMesh::Vertex::getBindingDescriptions();
	m_ve_pipeline = std::make_unique<VePipeline>(m_ve_device, m_shader_path, config);
}

void AabbDebugRenderSystem::createVertexBuffer() {
	const uint32_t max_vertices = MAX_AABB_BOXES * VERTICES_PER_BOX;
	m_vertex_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(VeMesh::Vertex),
		max_vertices,
		vk::BufferUsageFlagBits::eVertexBuffer,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
		1
	);
	m_vertex_buffer->map();
}

void AabbDebugRenderSystem::render(VeFrameInfo& frame_info) const {
	std::vector<VeMesh::Vertex> vertices;
	vertices.reserve(MAX_AABB_BOXES * VERTICES_PER_BOX);

	const glm::vec3 color(1.0f, 0.1f, 0.0f);

	for (auto& [id, entry] : frame_info.visible_game_objects) {
		MeshComponent* mesh_comp = entry.mesh;
		if (!mesh_comp || !mesh_comp->hasMesh())
			continue;

		VeMesh::AABB world_aabb = mesh_comp->getWorldAABB();
		addAABBEdges(world_aabb, color, vertices);

		if (vertices.size() >= MAX_AABB_BOXES * VERTICES_PER_BOX)
			break;
	}

	if (vertices.empty())
		return;

	m_vertex_buffer->writeToBuffer(vertices.data(), vertices.size() * sizeof(VeMesh::Vertex));

	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		{*frame_info.global_descriptor_set},
		{}
	);

	vk::Buffer buffers[] = {*m_vertex_buffer->getBuffer()};
	vk::DeviceSize offsets[] = {0};
	frame_info.command_buffer.bindVertexBuffers(0, buffers, offsets);
	frame_info.command_buffer.draw(static_cast<uint32_t>(vertices.size()), 1, 0, 0);
}

void AabbDebugRenderSystem::recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	m_color_format = color_format;
	m_sample_count = sample_count;
	m_ve_pipeline.reset();
	createPipeline(color_format, sample_count);
}

} // namespace ve
